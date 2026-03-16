# Define paths
$buildDir = "build"
$installDir = "$buildDir/install"
$binDir = "$buildDir/bin"
$shadersDir = "src/shaders"
$dataDir = "data"
$uiConfigDir = "src/ui/config"

# Clean install directory
if (Test-Path $installDir) {
    Write-Host "Cleaning install directory: $installDir"
    Remove-Item -Recurse -Force $installDir
}
New-Item -ItemType Directory -Path $installDir -Force

# Copy executable and DLLs
Write-Host "Copying executables and DLLs from $binDir to $installDir"
if (Test-Path $binDir) {
    New-Item -ItemType Directory -Path "$installDir" -Force
    Copy-Item -Path "$binDir/*" -Destination "$installDir" -Recurse
} else {
    Write-Error "Source directory $binDir does not exist."
    exit 1
}

# Copy compiled shaders
Write-Host "Copying SPIR-V shaders from $shadersDir to $installDir/$shadersDir"
if (Test-Path $shadersDir) {
    New-Item -ItemType Directory -Path "$installDir/$shadersDir" -Force
    Copy-Item -Path "$shadersDir/*.spv" -Destination "$installDir/$shadersDir" -Recurse
} else {
    Write-Error "Shaders directory $shadersDir does not exist."
    exit 1
}

# Copy data files
Write-Host "Copying data files from $dataDir to $installDir/data"
if (Test-Path $dataDir) {
    New-Item -ItemType Directory -Path "$installDir/data" -Force
    Copy-Item -Path "$dataDir/*" -Destination "$installDir/data" -Recurse
} else {
    Write-Error "Data directory $dataDir does not exist."
    exit 1
}

# Copy ImGui configuration files
Write-Host "Copying ImGui configuration files from $uiConfigDir to $installDir/$uiConfigDir"
if (Test-Path $uiConfigDir) {
    New-Item -ItemType Directory -Path "$installDir/$uiConfigDir" -Force
    Copy-Item -Path "$uiConfigDir/*.ini" -Destination "$installDir/$uiConfigDir" -Recurse
} else {
    Write-Error "ImGui configuration directory $uiConfigDir does not exist."
    exit 1
}

# Verify executables and DLLs
if (-Not (Test-Path "$installDir/triangle_sample.exe")) {
    Write-Error "Executable not found in $installDir"
    exit 1
}

# Verify shaders
if (-Not (Get-ChildItem "$installDir/$shadersDir/*.spv" -ErrorAction SilentlyContinue)) {
    Write-Error "No SPIR-V shaders found in $installDir/$shadersDir"
    exit 1
}

# Verify data files
if (-Not (Get-ChildItem "$installDir/data/*" -ErrorAction SilentlyContinue)) {
    Write-Error "No data files found in $installDir/data"
    exit 1
}

# Verify ImGui configuration file
if (-Not (Test-Path "$installDir/$uiConfigDir/*.ini" -ErrorAction SilentlyContinue)) {
    Write-Error "ImGui configuration file not found in $installDir/$uiConfigDir"
    exit 1
}

# Summary
$binCount = (Get-ChildItem "$installDir" -Recurse | Measure-Object).Count
$shaderCount = (Get-ChildItem "$installDir/$shadersDir" -Recurse | Measure-Object).Count
$dataCount = (Get-ChildItem "$installDir/data" -Recurse | Measure-Object).Count
$configCount = (Get-ChildItem "$installDir/$uiConfigDir" -Recurse | Measure-Object).Count

Write-Host "Packaging complete. Files copied to $installDir"
Write-Host "Copied $binCount files to $installDir/"
Write-Host "Copied $shaderCount shaders to $shadersDir/"
Write-Host "Copied $dataCount files to data/"
Write-Host "Copied $configCount ImGui configuration files to $uiConfigDir/"
