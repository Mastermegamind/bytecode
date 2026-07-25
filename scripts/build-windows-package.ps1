param(
    [string] $PhpDir = $env:PHP_DIR,
    [string] $OpdumpDll = $env:OPDUMP_DLL,
    [switch] $SkipExtension,
    [switch] $BuildMsi,
    [string] $Version = "1.0.0"
)

$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$DistDir = Join-Path $Root "dist"
$Bundle = Join-Path $Root "ui\build\windows\x64\runner\Release"
$PackageDir = Join-Path $DistDir "Bytecode_Encoder-windows-x64"
$ZipPath = Join-Path $DistDir "Bytecode_Encoder-windows-x64.zip"
$MsiPath = Join-Path $DistDir "Bytecode_Encoder-windows-x64.msi"
$WixSource = Join-Path $DistDir "Bytecode_Encoder.wxs"

function Convert-ToWixId {
    param([string] $Value)

    $id = $Value -replace '[^A-Za-z0-9_]', '_'
    if ($id -notmatch '^[A-Za-z_]') {
        $id = "I_$id"
    }
    if ($id.Length -gt 60) {
        $hash = [Math]::Abs($Value.GetHashCode())
        $id = $id.Substring(0, 48) + "_$hash"
    }
    return $id
}

function Convert-ToXmlAttribute {
    param([string] $Value)

    return [System.Security.SecurityElement]::Escape($Value)
}

function Write-WixDirectory {
    param(
        [System.IO.DirectoryInfo] $Directory,
        [string] $DirectoryId,
        [System.Collections.Generic.List[string]] $Lines,
        [System.Collections.Generic.List[string]] $ComponentIds,
        [int] $Depth
    )

    $indent = " " * $Depth
    foreach ($ChildDirectory in Get-ChildItem -LiteralPath $Directory.FullName -Directory | Sort-Object Name) {
        $childId = Convert-ToWixId ("DIR_" + $DirectoryId + "_" + $ChildDirectory.Name)
        $childName = Convert-ToXmlAttribute $ChildDirectory.Name
        $Lines.Add("$indent<Directory Id=""$childId"" Name=""$childName"">")
        Write-WixDirectory -Directory $ChildDirectory -DirectoryId $childId -Lines $Lines -ComponentIds $ComponentIds -Depth ($Depth + 2)
        $Lines.Add("$indent</Directory>")
    }

    foreach ($File in Get-ChildItem -LiteralPath $Directory.FullName -File | Sort-Object Name) {
        $relative = [System.IO.Path]::GetRelativePath($PackageDir, $File.FullName)
        $componentId = Convert-ToWixId ("CMP_" + $relative)
        $fileId = Convert-ToWixId ("FIL_" + $relative)
        $source = Convert-ToXmlAttribute $File.FullName
        $name = Convert-ToXmlAttribute $File.Name
        $Lines.Add("$indent<Component Id=""$componentId"" Guid=""*"">")
        $Lines.Add("$indent  <File Id=""$fileId"" Name=""$name"" Source=""$source"" KeyPath=""yes"" />")
        $Lines.Add("$indent</Component>")
        $ComponentIds.Add($componentId)
    }
}

Write-Host "== Build Flutter Windows app =="
Push-Location (Join-Path $Root "ui")
try {
    flutter build windows
}
finally {
    Pop-Location
}

Write-Host "== Assemble Windows package =="
if (Test-Path $PackageDir) {
    Remove-Item -Recurse -Force $PackageDir
}
if (Test-Path $ZipPath) {
    Remove-Item -Force $ZipPath
}
if (Test-Path $MsiPath) {
    Remove-Item -Force $MsiPath
}
New-Item -ItemType Directory -Force $PackageDir | Out-Null
Copy-Item -Recurse -Force (Join-Path $Bundle "*") $PackageDir

$BytecodeRoot = Join-Path $PackageDir "bytecode"
$BytecodePhp = Join-Path $BytecodeRoot "php"
$ModuleDir = Join-Path $BytecodePhp "src\modules"
New-Item -ItemType Directory -Force $ModuleDir | Out-Null
Copy-Item -Recurse -Force (Join-Path $Root "php\bin") $BytecodePhp

if (-not $SkipExtension) {
    if (-not $OpdumpDll) {
        $Candidates = @(
            (Join-Path $Root "php\src\x64\Release\php_opdump.dll"),
            (Join-Path $Root "php\src\Release\php_opdump.dll"),
            (Join-Path $Root "php\src\modules\opdump.dll")
        )
        foreach ($Candidate in $Candidates) {
            if (Test-Path $Candidate) {
                $OpdumpDll = $Candidate
                break
            }
        }
    }

    if (-not $OpdumpDll -or -not (Test-Path $OpdumpDll)) {
        throw "No Windows opdump extension found. Set OPDUMP_DLL=C:\path\to\php_opdump.dll, or rerun with -SkipExtension for a UI-only package."
    }

    Copy-Item -Force $OpdumpDll (Join-Path $ModuleDir "php_opdump.dll")
}

$PhpPackageDir = Join-Path $PackageDir "php"
if ($PhpDir -and (Test-Path $PhpDir)) {
    Copy-Item -Recurse -Force $PhpDir $PhpPackageDir
}
else {
    $PhpCommand = Get-Command "php.exe" -ErrorAction SilentlyContinue
    if ($PhpCommand) {
        New-Item -ItemType Directory -Force $PhpPackageDir | Out-Null
        Copy-Item -Force $PhpCommand.Source (Join-Path $PhpPackageDir "php.exe")
        Write-Warning "Only php.exe was copied. For a portable package, set PHP_DIR to a full PHP for Windows directory."
    }
    else {
        Write-Warning "No bundled php.exe found. The app will use php.exe from PATH unless PHP_DIR is provided."
    }
}

Write-Host "== Zip Windows package =="
Compress-Archive -Path (Join-Path $PackageDir "*") -DestinationPath $ZipPath -Force

if ($BuildMsi) {
    Write-Host "== Build Windows MSI =="
    $Wix = Get-Command "wix.exe" -ErrorAction SilentlyContinue
    if (-not $Wix) {
        $Wix = Get-Command "wix" -ErrorAction SilentlyContinue
    }
    if (-not $Wix) {
        throw "WiX Toolset was not found. Install with: dotnet tool install --global wix"
    }

    $componentIds = [System.Collections.Generic.List[string]]::new()
    $lines = [System.Collections.Generic.List[string]]::new()
    $upgradeCode = "8D1844F0-291B-4A45-95D4-A083E020E888"

    $lines.Add('<Wix xmlns="http://wixtoolset.org/schemas/v4/wxs">')
    $lines.Add("  <Package Name=""Bytecode Encoder"" Manufacturer=""Bytecode"" Version=""$Version"" UpgradeCode=""$upgradeCode"" Scope=""perMachine"">")
    $lines.Add('    <MajorUpgrade DowngradeErrorMessage="A newer version of Bytecode Encoder is already installed." />')
    $lines.Add('    <MediaTemplate EmbedCab="yes" />')
    $lines.Add('    <StandardDirectory Id="ProgramFiles64Folder">')
    $lines.Add('      <Directory Id="INSTALLFOLDER" Name="Bytecode Encoder">')
    Write-WixDirectory -Directory (Get-Item $PackageDir) -DirectoryId "INSTALLFOLDER" -Lines $lines -ComponentIds $componentIds -Depth 8
    $lines.Add('      </Directory>')
    $lines.Add('    </StandardDirectory>')
    $lines.Add('    <Feature Id="MainFeature" Title="Bytecode Encoder" Level="1">')
    foreach ($componentId in $componentIds) {
        $lines.Add("      <ComponentRef Id=""$componentId"" />")
    }
    $lines.Add('    </Feature>')
    $lines.Add('  </Package>')
    $lines.Add('</Wix>')
    Set-Content -Path $WixSource -Value $lines -Encoding UTF8

    & $Wix.Source build $WixSource -arch x64 -o $MsiPath
}

Write-Host ""
Write-Host "Windows package built:"
Write-Host $PackageDir
Write-Host $ZipPath
if ($BuildMsi) {
    Write-Host $MsiPath
}
