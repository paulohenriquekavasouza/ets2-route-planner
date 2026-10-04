# Build RoutePlanner, test it, regenerate routes.tsv from the game's own files, and install it,
# with the game open or closed.
#   core\RoutePlannerCore.dll  all the logic. Replaced atomically; the host notices the new file
#                              within a second and hot-reloads it. Nothing to do in game.
#   RoutePlanner.dll           the host (manifest, keys, windows). Only replaced when it actually
#                              changed; a running game keeps the old one until SPF -> "Recarregar
#                              Framework" or a restart.
$ErrorActionPreference = 'Stop'
$cmake   = 'E:\Program Files\Microsoft Visual Studio\18\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$plugin  = Join-Path $PSScriptRoot 'plugin'
$build   = Join-Path $plugin 'build'
$dest    = 'D:\SteamLibrary\steamapps\common\Euro Truck Simulator 2\bin\win_x64\plugins\spfPlugins\RoutePlanner'
$extract = 'C:\Users\Paulo\ets2-x2'   # sk-zk Extractor output: /def of every archive + /locale/pt_br (never in the repo)
$stale   = Join-Path $build 'stale'   # same volume as $dest

& $cmake -S $plugin -B $build -A x64 | Out-Null
$out = & $cmake --build $build --config Release
if ($LASTEXITCODE -ne 0) { $out | Select-String 'error' | ForEach-Object { $_.Line }; throw 'build failed' }
$out | Select-String 'warning C' | ForEach-Object { $_.Line }
Push-Location $build; & (Join-Path $build 'Release\routes_test.exe'); $rc = $LASTEXITCODE; Pop-Location
if ($rc -ne 0) { throw 'routes_test failed' }

New-Item -ItemType Directory -Force (Join-Path $dest 'core') | Out-Null
python (Join-Path $PSScriptRoot 'tools\gen_routes.py') $extract (Join-Path $dest 'routes.tsv')

# --- host first (so a new host never meets an old core layout for long): a loaded DLL cannot be
#     overwritten, but it can be moved within the same volume
$hostNew = Join-Path $build 'Release\RoutePlanner.dll'
$hostDst = Join-Path $dest 'RoutePlanner.dll'
$hostChanged = -not (Test-Path $hostDst) -or (Get-FileHash $hostNew).Hash -ne (Get-FileHash $hostDst).Hash
if ($hostChanged) {
  New-Item -ItemType Directory -Force $stale | Out-Null
  Get-ChildItem $stale -Filter *.dll | ForEach-Object { try { Remove-Item $_.FullName -ErrorAction Stop } catch {} }  # still-loaded ones stay
  try { Copy-Item $hostNew $hostDst -Force; $hostMsg = 'host deployed' }
  catch {
    Move-Item $hostDst (Join-Path $stale ("RoutePlanner-{0:yyyyMMdd-HHmmss}.dll" -f (Get-Date)))
    Copy-Item $hostNew $hostDst
    $hostMsg = 'HOST CHANGED while the game is running: in game, SPF -> Recarregar Framework (or restart) to pick it up.'
  }
} else { $hostMsg = 'host unchanged' }

# --- core: copy next to the target, then rename over it so the host never sees a half-written file
$coreNew = Join-Path $dest 'core\RoutePlannerCore.dll.new'
Copy-Item (Join-Path $build 'Release\RoutePlannerCore.dll') $coreNew -Force
Move-Item $coreNew (Join-Path $dest 'core\RoutePlannerCore.dll') -Force
'core deployed (hot reload)'
$hostMsg
