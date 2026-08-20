const manifestUrl = "https://raw.githubusercontent.com/1737894252/AstraView/main/update.json";

function formatBytes(bytes) {
  if (!Number.isFinite(bytes) || bytes <= 0) return "离线安装包";
  return `${(bytes / 1024 / 1024).toFixed(1)} MB 离线安装包`;
}

fetch(`${manifestUrl}?v=${Date.now()}`, { cache: "no-store" })
  .then(response => {
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    return response.json();
  })
  .then(manifest => {
    document.querySelectorAll(".version-text").forEach(node => {
      node.textContent = `v${manifest.version}`;
    });
    document.querySelectorAll(".download-link").forEach(link => {
      link.href = manifest.downloadUrl;
    });
    const size = document.querySelector(".file-size");
    if (size) size.textContent = formatBytes(manifest.size);
    const sha = document.querySelector(".sha-text");
    if (sha) sha.textContent = manifest.sha256;
  })
  .catch(() => {
    // Static fallbacks remain usable if the manifest is temporarily unavailable.
  });
