@echo off
setlocal

set SCENE_TOOL="D:\PersonalProjects\BudEngine\build\bin\debug\SceneTool.exe"
set IMPORTER="D:\PersonalProjects\BudEngine\build\bin\debug\BudAssetImporter.exe"


set SOURCE_MODEL="D:\PersonalProjects\data\cryteksponza\sponza.obj"
set TARGET_DIR=Content/cryteksponza
set TARGET_SCENE=Content/Scenes/sponza_page_scene.json

echo [ImportAsset] Creating scene: %TARGET_SCENE%
%SCENE_TOOL% --create --name %TARGET_SCENE% --template default
if %errorlevel% neq 0 (
    echo [ImportAsset] Warning: SceneTool returned non-zero, scene may already exist or creation failed.
)

echo [ImportAsset] Importing asset: %SOURCE_MODEL% into scene: %TARGET_SCENE% at directory: %TARGET_DIR%
%IMPORTER% --input %SOURCE_MODEL% --output-dir %TARGET_DIR% --scene %TARGET_SCENE% --no-cache --dump-text
if %errorlevel% neq 0 (
    exit /b 1
)

endlocal
