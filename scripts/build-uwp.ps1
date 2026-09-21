#Requires -Version 7.0
param([ValidateRange(0,65535)][int]$Revision = 0)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
$Root = Split-Path $PSScriptRoot -Parent
if (-not $IsWindows) { throw 'Run packaging on Windows; use GitHub Actions from the Mac.' }
$Sdk = "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.0.22621.0\x64"
$VsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$MsBuild = & $VsWhere -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $MsBuild) { throw 'Visual Studio 2022 / MSBuild not found' }
foreach ($name in @('fxc.exe','dxc.exe','makeappx.exe','signtool.exe')) {
    if (-not (Test-Path "$Sdk\$name")) { throw "Windows SDK tool missing: $name" }
}
$Output = Join-Path $Root 'out'
$Generated = Join-Path $Root 'uwp\Generated'
New-Item -ItemType Directory -Force $Output,$Generated | Out-Null

# Offline correctness tests, not mining. No pool or account configuration in CI.
& cmake -S $Root -B "$Root\build-windows" -A x64
& cmake --build "$Root\build-windows" --config Release --parallel 2
& ctest --test-dir "$Root\build-windows" -C Release --output-on-failure

# Shader is compiled before packaging. No runtime shader compiler/JIT in UWP.
& "$Sdk\fxc.exe" /nologo /T cs_5_1 /E CSMain /O3 /Ges /WX `
    /Fh "$Generated\matmul_shader.h" /Vn g_matmul_shader `
    "$Root\shaders\matmul_transcript.hlsl"
# Keep compiler, packed layout, dot-product, and wave effects distinguishable.
# cs_6_4 selects the older DXIL contract accepted by the documented Xbox UWP
# feature envelope; capability and pipeline creation are also checked on Xbox.
$Variants = @(
    @{ Name='dxc_scalar'; Packed=0; Dot4=0; Wave=0 },
    @{ Name='dxc_packed_scalar'; Packed=1; Dot4=0; Wave=0 },
    @{ Name='dxc_dot4'; Packed=1; Dot4=1; Wave=0 },
    @{ Name='dxc_wave'; Packed=0; Dot4=0; Wave=1 },
    @{ Name='dxc_dot4_wave'; Packed=1; Dot4=1; Wave=1 }
)
foreach ($Variant in $Variants) {
    $Name = $Variant.Name
    & "$Sdk\dxc.exe" -T cs_6_4 -E CSMain -O3 -Ges -WX -HV 2018 `
        -validator-version 1.4 `
        -D "PEARL_PACKED=$($Variant.Packed)" -D "PEARL_DOT4=$($Variant.Dot4)" -D "PEARL_WAVE=$($Variant.Wave)" `
        -Fh "$Generated\matmul_$Name.h" -Vn "g_matmul_$Name" `
        -Fc "$Output\shader-$Name.asm" "$Root\shaders\matmul_transcript.hlsl"
}
& nuget restore "$Root\uwp\packages.config" -PackagesDirectory "$Root\uwp\packages" -NonInteractive

[xml]$Manifest = Get-Content "$Root\uwp\AppxManifest.xml"
$Manifest.Package.Identity.Version = "0.1.0.$Revision"
$Manifest.Save("$Root\uwp\AppxManifest.xml")
& $MsBuild "$Root\uwp\PearlProbe.vcxproj" /m:2 /p:Configuration=Release /p:Platform=x64 `
    /p:AppxPackageSigningEnabled=false /p:AppxBundle=Never /p:UapAppxPackageBuildMode=SideloadOnly `
    /p:GenerateAppxPackageOnBuild=true "/p:AppxPackageDir=$Output\" /verbosity:minimal

$Package = Get-ChildItem $Output -Recurse -File | Where-Object {
    $_.Extension -in @('.msix','.appx') -and $_.Name -like '*PearlProbe*'
} | Select-Object -First 1
if (-not $Package) { throw 'MSBuild produced no PearlProbe package' }

# Ephemeral signing certificate for local Dev Mode. The private key remains on
# this Windows builder; only the public certificate is included in artifacts.
$Certificate = New-SelfSignedCertificate -Type Custom -Subject 'CN=pearl-probe-dev' `
    -KeyUsage DigitalSignature -CertStoreLocation 'Cert:\CurrentUser\My' `
    -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3','2.5.29.19={text}')
& "$Sdk\signtool.exe" sign /fd SHA256 /sha1 $Certificate.Thumbprint $Package.FullName
Export-Certificate -Cert $Certificate -FilePath "$Output\pearl-probe.cer" | Out-Null
$FinalPackage = Join-Path $Output 'PearlProbe.msix'
Copy-Item $Package.FullName $FinalPackage -Force
$Sums = @($FinalPackage,"$Output\pearl-probe.cer") | ForEach-Object {
    $Hash = (Get-FileHash $_ -Algorithm SHA256).Hash.ToLowerInvariant()
    "$Hash  $(Split-Path $_ -Leaf)"
}
$Sums | Set-Content "$Output\SHA256SUMS.txt"
Write-Host 'Prepared V3 diagnostic and optional private pool session. CI did not connect or mine.'
