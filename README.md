# AstraView

AstraView 是一款面向 Windows 10/11 x64 的现代图片查看器，专注于快速浏览常见图片、设计源文件、相机 RAW 和 PDF，同时为 Windows 资源管理器提供缩略图扩展。

## 功能

- 打开单张图片，或按目录浏览图片与子目录
- 底部缩略图胶片栏、上一张/下一张和最近目录
- 目录内容变化自动刷新
- 鼠标滚轮缩放、拖拽平移、适应窗口、1:1 和旋转
- 无边框深色界面，多显示器最大化时避让任务栏
- PSD/PSB 合成图显示，使用 Magick.NET Q8 控制大文件内存占用
- PDF 首页渲染及缩略图，基于 PDFium 和 SkiaSharp
- Windows Explorer 64 位 `IThumbnailProvider` COM 扩展
- 双击打开、文件关联及可选的“使用 AstraView 打开”右键菜单
- 离线安装包内含 .NET Framework 4.8，支持覆盖更新和标准 Windows 卸载

## 支持格式

覆盖 JPEG、PNG、GIF、BMP、ICO、TIFF、WebP、AVIF、HEIC/HEIF、SVG、PSD/PSB、EXR、DDS、TGA、PDF，以及 CR2/CR3、NEF、ARW、DNG、RAF、RW2、ORF 等常见 RAW 格式。

部分厂商私有格式、损坏文件或特殊编码可能无法解码。PSD/PSB 显示文档的合成结果，不提供图层编辑；PDF 当前显示第一页。

## 技术栈

- WPF / .NET Framework 4.8
- Magick.NET Q8 x64
- PDFium / PDFtoImage / SkiaSharp
- Windows Shell COM `IThumbnailProvider`
- Inno Setup 6

## 构建

环境要求：

- Windows 10/11 x64
- .NET 8 SDK（用于构建目标为 `net48` 的项目）
- Inno Setup 6（仅制作安装包时需要）

发布主程序与缩略图组件：

```powershell
.\scripts\publish.ps1
```

输出目录：`artifacts\publish`

制作包含 .NET Framework 4.8 离线运行时的安装包：

```powershell
.\scripts\build-installer.ps1
```

输出目录：`artifacts\installer`

> `installer/redist/NDP48-x86-x64-AllOS-ENU.exe` 是本地构建依赖，不纳入 Git 仓库。

## 快捷键

| 操作 | 快捷键 |
|---|---|
| 打开图片 | `Ctrl+O` |
| 打开文件夹 | `Ctrl+Shift+O` |
| 上一张 / 下一张 | `←` / `→` |
| 缩放 | 鼠标滚轮 |
| 平移 | 鼠标左键拖拽 |
| 最大化 / 恢复 | `F11` / `Esc` |

## Windows Explorer 集成

缩略图提供器运行在 64 位 Explorer 进程中，因此程序和 COM 组件固定构建为 x64。安装与卸载通过 `SHChangeNotify` 通知资源管理器刷新关联，不会强制终止 Explorer。

安装程序不会擅自替换 Windows 默认图片应用。用户可以通过“打开方式”将 AstraView 设为特定格式的默认程序，也可以在安装时选择添加右键菜单。

## 发布说明

公开分发前建议为主程序、原生/托管组件和最终安装包添加 Authenticode 代码签名。Microsoft Store 的传统 EXE 提交还要求所有 PE 文件均使用受信任证书签名。
