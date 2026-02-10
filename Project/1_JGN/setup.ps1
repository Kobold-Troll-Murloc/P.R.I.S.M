# OGRE Next One-Click Setup Script for P.R.I.S.M (V3 - Dependency Path Fix)
$ErrorActionPreference = "Stop"

$RootDir = $PSScriptRoot
$DepsDir = Join-Path $RootDir "ogre-next-deps"
$EngineDir = Join-Path $RootDir "ogre-next"
$VisualStudioVersion = "Visual Studio 17 2022"

function Build-Project {
    param (
        [string]$Path,
        [string]$BuildDirName = "build",
        [hashtable]$CmakeArgs = @{}
    )
    
    $BuildPath = Join-Path $Path $BuildDirName
    if (Test-Path $BuildPath) {
        Write-Host "--- Cleaning existing build directory: $BuildPath ---" -ForegroundColor Gray
        Remove-Item -Recurse -Force $BuildPath
    }
    New-Item -ItemType Directory -Path $BuildPath | Out-Null
    
    Push-Location $Path
    try {
        Write-Host "--- Configuring project in $Path ---" -ForegroundColor Cyan
        $ArgString = ""
        foreach ($Key in $CmakeArgs.Keys) {
            $ArgString += " -D$Key=$($CmakeArgs[$Key])"
        }
        
        $configCmd = "cmake -B $BuildDirName -G `"$VisualStudioVersion`" -A x64 $ArgString"
        Invoke-Expression $configCmd
        if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed for $Path" }
        
        Write-Host "--- Building and Installing (Release) ---" -ForegroundColor Cyan
        cmake --build $BuildDirName --config Release --target INSTALL --parallel 8
        if ($LASTEXITCODE -ne 0) { throw "Release build failed for $Path" }
        
        Write-Host "--- Building and Installing (Debug) ---" -ForegroundColor Cyan
        cmake --build $BuildDirName --config Debug --target INSTALL --parallel 8
        if ($LASTEXITCODE -ne 0) { throw "Debug build failed for $Path" }
    }
    finally {
        Pop-Location
    }
}

Write-Host "==========================================" -ForegroundColor Magenta
Write-Host "   Starting P.R.I.S.M Environment Setup   " -ForegroundColor Magenta
Write-Host "==========================================" -ForegroundColor Magenta

try {
    # 1. Build ogre-next-deps
    Write-Host "`n[1/2] Building Dependencies..." -ForegroundColor Yellow
    Build-Project -Path $DepsDir

    # 2. Build ogre-next
    Write-Host "`n[2/2] Building OGRE Next Engine..." -ForegroundColor Yellow
    $OgreDepsPath = (Join-Path $DepsDir "build/ogredeps").Replace('\', '/')
    $OgreInstallPath = (Join-Path $EngineDir "build/sdk").Replace('\', '/')

    $OgreArgs = @{
        "OGRE_BUILD_SAMPLES2" = "ON";
        "OGREDEPS_PATH" = "`"$OgreDepsPath`"";
        "CMAKE_INSTALL_PREFIX" = "`"$OgreInstallPath`"";
        "CMAKE_PREFIX_PATH" = "`"$OgreDepsPath`""; # 추가: CMake 패키지 검색 경로
        "CMAKE_FRAMEWORK_PATH" = "`"$OgreDepsPath`"" # 추가
    }
    
    # 추가 팁: OgreNext는 환경변수 OGRE_DEPENDENCIES_DIR 도 참조합니다.
    $env:OGRE_DEPENDENCIES_DIR = $OgreDepsPath

    Build-Project -Path $EngineDir -CmakeArgs $OgreArgs

    Write-Host "`n==========================================" -ForegroundColor Green
    Write-Host "   Setup Completed Successfully!          " -ForegroundColor Green
    Write-Host "==========================================" -ForegroundColor Green
}
catch {
    Write-Host "`n******************************************" -ForegroundColor Red
    Write-Host "   ERROR: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "   Setup failed. Please check the logs.   " -ForegroundColor Red
    Write-Host "******************************************" -ForegroundColor Red
    exit 1
}
