# 检查安装目录能独立启动并加载自身的翻译，不从构建目录或工具链补充 DLL。
param(
    [Parameter(Mandatory=$true)][string]$InstallDirectory,
    [Parameter(Mandatory=$true)][string]$BuildDirectory
)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()
$scrctlInstall = (Resolve-Path $InstallDirectory).Path
$scrctlBuild = (Resolve-Path $BuildDirectory).Path
$scrctlMoved = Join-Path ([System.IO.Path]::GetTempPath()) ('scrctl-install-' + [guid]::NewGuid())
$scrctlCatalog = Join-Path $scrctlBuild 'locale'
$scrctlHidden = Join-Path $scrctlBuild ('locale-hidden-' + [guid]::NewGuid())
$scrctlEnvironment = @{}
foreach ($scrctlName in @('Path', 'LC_ALL', 'LC_MESSAGES', 'LANG', 'SCRCTL_LOCALEDIR')) {
    $scrctlEnvironment[$scrctlName] = [Environment]::GetEnvironmentVariable($scrctlName, 'Process')
}
$scrctlCatalogHidden = $false
try {
    Copy-Item $scrctlInstall $scrctlMoved -Recurse
    if (Test-Path $scrctlCatalog) {
        Move-Item $scrctlCatalog $scrctlHidden
        $scrctlCatalogHidden = $true
    }
    $env:Path = "$env:SystemRoot\System32;$env:SystemRoot"
    $env:SCRCTL_LOCALEDIR = ''
    $env:LC_ALL = 'C'
    $env:LC_MESSAGES = ''
    $env:LANG = 'zh_CN.UTF-8'
    $scrctlExe = Join-Path $scrctlMoved 'bin\scrctl.exe'
    $scrctlHelp = & $scrctlExe --help
    if ($LASTEXITCODE -ne 0 -or !($scrctlHelp -match 'iOS screen mirroring and control')) {
        throw 'Installed executable failed to start with English fallback'
    }
    $env:LC_ALL = 'zh_CN.UTF-8'
    $scrctlHelp = & $scrctlExe --help
    if ($LASTEXITCODE -ne 0 -or !($scrctlHelp -match 'iOS 屏幕镜像与控制')) {
        throw 'Installed executable failed to load its Chinese catalog'
    }
    & $scrctlExe --version
    if ($LASTEXITCODE -ne 0) { throw 'Installed executable failed to print its version' }
    Write-Output 'Relocated Windows install: startup and automatic language selection passed'
} finally {
    if ($scrctlCatalogHidden) { Move-Item $scrctlHidden $scrctlCatalog }
    foreach ($scrctlName in $scrctlEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($scrctlName, $scrctlEnvironment[$scrctlName], 'Process')
    }
    if (Test-Path $scrctlMoved) { Remove-Item $scrctlMoved -Recurse -Force }
}
