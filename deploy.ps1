# Build RoutePlanner, test it, regenerate routes.tsv from the game's own files, and install it.
# A running game keeps the old DLL until SPF -> "Recarregar Framework" (or a restart).
$ErrorActionPreference = 'Stop'
$cmake   = 'E:\Program Files\Microsoft Visual Studio\18\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$plugin  = Join-Path $PSScriptRoot 'plugin'
$build   = Join-Path $plugin 'build'
$dest    = 'D:\SteamLibrary\steamapps\common\Euro Truck Simulator 2\bin\win_x64\plugins\spfPlugins\RoutePlanner'
$extract = 'C:\Users\Paulo\ets2-x2'   # sk-zk Extractor output: /def of every archive + /locale/pt_br (never in the repo)
$stale   = Join-Path $build 'stale'         # same volume as $dest

& $cmake -S $plugin -B $build -A x64 | Out-Null
$out = & $cmake --build $build --config Release
if ($LASTEXITCODE -ne 0) { $out | Select-String 'error' | ForEach-Object { $_.Line }; throw 'build failed' }
$out | Select-String 'warning C' | ForEach-Object { $_.Line }
Push-Location $build; & (Join-Path $build 'Release\routes_test.exe'); $rc = $LASTEXITCODE; Pop-Location
if ($rc -ne 0) { throw 'routes_test failed' }

New-Item -ItemType Directory -Force $dest | Out-Null
python (Join-Path $PSScriptRoot 'tools\gen_routes.py') $extract (Join-Path $dest 'routes.tsv')

$new = Join-Path $build 'Release\RoutePlanner.dll'
$dst = Join-Path $dest 'RoutePlanner.dll'
if (-not (Test-Path $dst) -or (Get-FileHash $new).Hash -ne (Get-FileHash $dst).Hash) {
  New-Item -ItemType Directory -Force $stale | Out-Null
  Get-ChildItem $stale -Filter *.dll | ForEach-Object { try { Remove-Item $_.FullName -ErrorAction Stop } catch {} }
  try { Copy-Item $new $dst -Force; 'dll deployed' }
  catch {
    Move-Item $dst (Join-Path $stale ("RoutePlanner-{0:yyyyMMdd-HHmmss}.dll" -f (Get-Date)))
    Copy-Item $new $dst
    'DLL CHANGED while the game is running: SPF -> Recarregar Framework (or restart).'
  }
} else { 'dll unchanged' }
