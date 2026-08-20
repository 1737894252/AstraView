using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Reflection;
using System.Runtime.Serialization;
using System.Runtime.Serialization.Json;
using System.Security.Cryptography;
using System.Threading;
using System.Threading.Tasks;

namespace StarImageViewer;

internal sealed class UpdateInfo
{
    public Version Version { get; init; } = new Version();
    public string TagName { get; init; } = string.Empty;
    public string ReleaseName { get; init; } = string.Empty;
    public string ReleaseNotes { get; init; } = string.Empty;
    public string DownloadUrl { get; init; } = string.Empty;
    public string ApiUrl { get; init; } = string.Empty;
    public string FileName { get; init; } = string.Empty;
    public string Sha256 { get; init; } = string.Empty;
    public long Size { get; init; }
}

internal static class UpdateService
{
    private const string UpdateManifestUrl = "https://raw.githubusercontent.com/1737894252/AstraView/main/update.json";
    private const string LatestReleaseApi = "https://api.github.com/repos/1737894252/AstraView/releases/latest";
    private const string InstallerPrefix = "AstraView-Setup-";
    private static readonly HttpClient Client = CreateClient();

    public static Version CurrentVersion =>
        Assembly.GetEntryAssembly()?.GetName().Version ?? new Version(1, 0, 0);

    public static async Task<UpdateInfo?> CheckAsync(CancellationToken cancellationToken)
    {
        try
        {
            return await CheckManifestAsync(cancellationToken);
        }
        catch (HttpRequestException)
        {
            // Keep the GitHub API as a compatibility fallback, but normal update checks do not spend
            // the anonymous API quota shared by users behind the same public IP address.
        }
        catch (InvalidDataException)
        {
            // A temporarily incomplete manifest must not permanently disable the fallback channel.
        }

        using var request = new HttpRequestMessage(HttpMethod.Get, LatestReleaseApi);
        AddPrivateRepositoryToken(request);
        using var response = await Client.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, cancellationToken);
        if (response.StatusCode == HttpStatusCode.NotFound)
            throw new InvalidOperationException("更新服务当前不可公开访问。请确认 GitHub 仓库已公开，或为测试环境设置 ASTRAVIEW_GITHUB_TOKEN。");

        if (response.StatusCode == (HttpStatusCode)429 || response.StatusCode == HttpStatusCode.Forbidden)
            throw new InvalidOperationException("GitHub 更新接口暂时限流，请稍后重试。");
        response.EnsureSuccessStatusCode();
        var release = ReadJson<GitHubRelease>(await response.Content.ReadAsStreamAsync());
        if (release.Draft || release.Prerelease || !TryParseVersion(release.TagName, out var version)) return null;

        var asset = release.Assets.FirstOrDefault(item =>
            item.Name.StartsWith(InstallerPrefix, StringComparison.OrdinalIgnoreCase) &&
            item.Name.EndsWith("-x64.exe", StringComparison.OrdinalIgnoreCase));
        if (asset == null) throw new InvalidOperationException("最新版本没有找到 x64 安装文件。");

        return new UpdateInfo
        {
            Version = version,
            TagName = release.TagName,
            ReleaseName = string.IsNullOrWhiteSpace(release.Name) ? release.TagName : release.Name,
            ReleaseNotes = release.Body ?? string.Empty,
            DownloadUrl = asset.BrowserDownloadUrl,
            ApiUrl = asset.Url,
            FileName = asset.Name,
            Sha256 = NormalizeDigest(asset.Digest),
            Size = asset.Size
        };
    }

    private static async Task<UpdateInfo?> CheckManifestAsync(CancellationToken cancellationToken)
    {
        using var request = new HttpRequestMessage(HttpMethod.Get, UpdateManifestUrl);
        request.Headers.Accept.Clear();
        request.Headers.Accept.ParseAdd("application/json");
        request.Headers.CacheControl = new System.Net.Http.Headers.CacheControlHeaderValue { NoCache = true };
        using var response = await Client.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, cancellationToken);
        response.EnsureSuccessStatusCode();
        var manifest = ReadJson<UpdateManifest>(await response.Content.ReadAsStreamAsync());
        if (!TryParseVersion(manifest.Version, out var version) ||
            string.IsNullOrWhiteSpace(manifest.DownloadUrl) ||
            string.IsNullOrWhiteSpace(manifest.FileName))
            throw new InvalidDataException("更新清单格式无效。");

        return new UpdateInfo
        {
            Version = version,
            TagName = "v" + version.ToString(3),
            ReleaseName = string.IsNullOrWhiteSpace(manifest.ReleaseName)
                ? "AstraView " + version.ToString(3) : manifest.ReleaseName,
            ReleaseNotes = manifest.ReleaseNotes ?? string.Empty,
            DownloadUrl = manifest.DownloadUrl,
            FileName = manifest.FileName,
            Sha256 = NormalizeDigest(manifest.Sha256),
            Size = manifest.Size
        };
    }

    public static bool IsNewer(UpdateInfo update) => update.Version.CompareTo(CurrentVersion) > 0;

    public static async Task<string> DownloadAsync(
        UpdateInfo update,
        IProgress<int>? progress,
        CancellationToken cancellationToken)
    {
        var updateDirectory = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "AstraView", "Updates");
        Directory.CreateDirectory(updateDirectory);
        var destination = Path.Combine(updateDirectory, update.FileName);
        var partial = destination + ".download";

        var hasPrivateAssetApi = !string.IsNullOrWhiteSpace(update.ApiUrl) &&
            !string.IsNullOrWhiteSpace(Environment.GetEnvironmentVariable("ASTRAVIEW_GITHUB_TOKEN"));
        using var request = new HttpRequestMessage(HttpMethod.Get,
            hasPrivateAssetApi ? update.ApiUrl : update.DownloadUrl);
        request.Headers.Accept.ParseAdd("application/octet-stream");
        AddPrivateRepositoryToken(request);
        using var response = await Client.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, cancellationToken);
        response.EnsureSuccessStatusCode();

        var total = response.Content.Headers.ContentLength ?? update.Size;
        using (var input = await response.Content.ReadAsStreamAsync())
        using (var output = new FileStream(partial, FileMode.Create, FileAccess.Write, FileShare.None, 81920, true))
        {
            var buffer = new byte[81920];
            long received = 0;
            int read;
            while ((read = await input.ReadAsync(buffer, 0, buffer.Length, cancellationToken)) > 0)
            {
                await output.WriteAsync(buffer, 0, read, cancellationToken);
                received += read;
                if (total > 0) progress?.Report((int)Math.Min(100, received * 100 / total));
            }
        }

        if (update.Size > 0 && new FileInfo(partial).Length != update.Size)
        {
            File.Delete(partial);
            throw new InvalidDataException("更新文件大小不正确，请重新检查更新。");
        }

        if (!string.IsNullOrEmpty(update.Sha256) && !HashMatches(partial, update.Sha256))
        {
            File.Delete(partial);
            throw new InvalidDataException("更新文件完整性校验失败，已取消安装。");
        }

        if (File.Exists(destination)) File.Delete(destination);
        File.Move(partial, destination);
        return destination;
    }

    public static void StartInstaller(string installerPath)
    {
        Process.Start(new ProcessStartInfo
        {
            FileName = installerPath,
            Arguments = "/SP- /SILENT /SUPPRESSMSGBOXES /CLOSEAPPLICATIONS /RESTARTAPPLICATIONS",
            UseShellExecute = true,
            Verb = "runas"
        });
    }

    private static HttpClient CreateClient()
    {
        var client = new HttpClient { Timeout = TimeSpan.FromMinutes(20) };
        client.DefaultRequestHeaders.UserAgent.ParseAdd("AstraView-Updater/1.1");
        client.DefaultRequestHeaders.Accept.ParseAdd("application/vnd.github+json");
        return client;
    }

    private static void AddPrivateRepositoryToken(HttpRequestMessage request)
    {
        var token = Environment.GetEnvironmentVariable("ASTRAVIEW_GITHUB_TOKEN");
        if (!string.IsNullOrWhiteSpace(token))
            request.Headers.Authorization = new System.Net.Http.Headers.AuthenticationHeaderValue("Bearer", token.Trim());
    }

    private static T ReadJson<T>(Stream stream)
    {
        var serializer = new DataContractJsonSerializer(typeof(T));
        return (T)(serializer.ReadObject(stream) ?? throw new InvalidDataException("更新信息格式无效。"));
    }

    private static bool TryParseVersion(string tag, out Version version)
    {
        var parsed = Version.TryParse((tag ?? string.Empty).Trim().TrimStart('v', 'V'), out var candidate);
        version = candidate ?? new Version();
        return parsed && candidate != null;
    }

    private static string NormalizeDigest(string? digest)
    {
        if (string.IsNullOrWhiteSpace(digest)) return string.Empty;
        var separator = digest!.IndexOf(':');
        return (separator >= 0 ? digest.Substring(separator + 1) : digest).Trim();
    }

    private static bool HashMatches(string path, string expected)
    {
        using var sha = SHA256.Create();
        using var stream = File.OpenRead(path);
        var actual = BitConverter.ToString(sha.ComputeHash(stream)).Replace("-", string.Empty);
        return actual.Equals(expected, StringComparison.OrdinalIgnoreCase);
    }

    [DataContract]
    private sealed class UpdateManifest
    {
        [DataMember(Name = "version")] public string Version { get; set; } = string.Empty;
        [DataMember(Name = "releaseName")] public string ReleaseName { get; set; } = string.Empty;
        [DataMember(Name = "releaseNotes")] public string? ReleaseNotes { get; set; }
        [DataMember(Name = "downloadUrl")] public string DownloadUrl { get; set; } = string.Empty;
        [DataMember(Name = "fileName")] public string FileName { get; set; } = string.Empty;
        [DataMember(Name = "sha256")] public string Sha256 { get; set; } = string.Empty;
        [DataMember(Name = "size")] public long Size { get; set; }
    }

    [DataContract]
    private sealed class GitHubRelease
    {
        [DataMember(Name = "tag_name")] public string TagName { get; set; } = string.Empty;
        [DataMember(Name = "name")] public string Name { get; set; } = string.Empty;
        [DataMember(Name = "body")] public string? Body { get; set; }
        [DataMember(Name = "draft")] public bool Draft { get; set; }
        [DataMember(Name = "prerelease")] public bool Prerelease { get; set; }
        [DataMember(Name = "assets")] public GitHubAsset[] Assets { get; set; } = Array.Empty<GitHubAsset>();
    }

    [DataContract]
    private sealed class GitHubAsset
    {
        [DataMember(Name = "name")] public string Name { get; set; } = string.Empty;
        [DataMember(Name = "url")] public string Url { get; set; } = string.Empty;
        [DataMember(Name = "browser_download_url")] public string BrowserDownloadUrl { get; set; } = string.Empty;
        [DataMember(Name = "digest")] public string? Digest { get; set; }
        [DataMember(Name = "size")] public long Size { get; set; }
    }
}
