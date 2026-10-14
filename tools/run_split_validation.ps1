[CmdletBinding()]
param(
    [string]$SettingsPath = '',
    [ValidateSet('Split', 'PluginUnlock', 'PluginLimiter', 'ClockTelemetry', 'CameraTrace', 'CameraRestore', 'CameraInterpolation', 'HandsTrace', 'HandsRenderCopy', 'HandsInterpolation', 'HandsPoseTrace', 'HandsPoseRestore', 'HandsPoseRenderCopy', 'HandsPoseInterpolation', 'HandsRestore', 'WorldTrace', 'WorldInterpolation', 'Play')]
    [string]$Mode = 'Split',
    [switch]$KeepTestSettings,
    # Play mode only: stage config\AmnesiaTheBunker.FPSFix.<name>.ini instead
    # of the play profile, for comparison runs.
    [string]$PluginProfile = '',
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Find-SettingsPath {
    $relative = Join-Path 'My Games\Amnesia The Bunker\Main' '*_user_settings.cfg'
    $roots = [System.Collections.Generic.List[string]]::new()
    $roots.Add((Join-Path $env:USERPROFILE 'Documents'))
    if ($env:OneDrive) {
        $roots.Add((Join-Path $env:OneDrive 'Documents'))
        $roots.Add((Join-Path $env:OneDrive 'Documenti'))
    }

    $matches = foreach ($root in $roots | Select-Object -Unique) {
        Get-ChildItem -Path (Join-Path $root $relative) -File -ErrorAction SilentlyContinue
    }
    $selected = $matches | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $selected) {
        throw 'Could not locate *_user_settings.cfg for Amnesia The Bunker.'
    }
    return $selected.FullName
}

function Set-ConfigAttribute {
    param(
        [Parameter(Mandatory)] [string]$Text,
        [Parameter(Mandatory)] [string]$Element,
        [Parameter(Mandatory)] [string]$Attribute,
        [Parameter(Mandatory)] [string]$Value
    )

    $pattern = '(<{0}\b[^>]*\b{1}=")[^"]*(")' -f
        [regex]::Escape($Element), [regex]::Escape($Attribute)
    $match = [regex]::Match(
        $Text, $pattern,
        [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
    if (-not $match.Success) {
        throw "Could not find $Element.$Attribute in the user settings."
    }
    $replacement = $match.Groups[1].Value + $Value + $match.Groups[2].Value
    return $Text.Substring(0, $match.Index) + $replacement +
        $Text.Substring($match.Index + $match.Length)
}

function New-SplitProfile {
    param(
        [Parameter(Mandatory)] [string]$Original,
        [Parameter(Mandatory)] [bool]$LimitFps
    )

    $result = Set-ConfigAttribute $Original 'Screen' 'RefreshRate' '240'
    $result = Set-ConfigAttribute $result 'Screen' 'Vsync' 'false'
    $result = Set-ConfigAttribute $result 'Engine' 'GameplayUpdateRate' '120'
    $limitValue = if ($LimitFps) { 'true' } else { 'false' }
    $result = Set-ConfigAttribute $result 'Engine' 'LimitFPS' $limitValue
    return $result
}

function Assert-Profile {
    param(
        [Parameter(Mandatory)] [string]$Text,
        [Parameter(Mandatory)] [bool]$LimitFps
    )

    $expectedLimit = if ($LimitFps) { 'true' } else { 'false' }
    $checks = @(
        '<Screen\b[^>]*\bRefreshRate="240"',
        '<Screen\b[^>]*\bVsync="false"',
        '<Engine\b[^>]*\bGameplayUpdateRate="120"',
        ('<Engine\b[^>]*\bLimitFPS="{0}"' -f $expectedLimit)
    )
    foreach ($check in $checks) {
        if ($Text -notmatch $check) {
            throw "Generated profile failed validation: $check"
        }
    }
}

function Write-Utf8NoBom {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [string]$Text
    )
    $encoding = [System.Text.UTF8Encoding]::new($false)
    [System.IO.File]::WriteAllText($Path, $Text, $encoding)
}

if (-not $SettingsPath) {
    $SettingsPath = Find-SettingsPath
}
$SettingsPath = [System.IO.Path]::GetFullPath($SettingsPath)
if (-not [System.IO.File]::Exists($SettingsPath)) {
    throw "Settings file does not exist: $SettingsPath"
}

$utf8 = [System.Text.UTF8Encoding]::new($false, $true)
$originalBytes = [System.IO.File]::ReadAllBytes($SettingsPath)
$originalText = $utf8.GetString($originalBytes)
$stockProfile = New-SplitProfile $originalText $true
$unlockedProfile = New-SplitProfile $originalText $false
Assert-Profile $stockProfile $true
Assert-Profile $unlockedProfile $false

if ($DryRun) {
    Write-Host 'Dry run passed: both split profiles were generated and validated.'
    Write-Host "Settings: $SettingsPath"
    exit 0
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$gameExe = 'D:\Program Files\Games\GOG\Amnesia The Bunker\AmnesiaTheBunker.exe'
$binDirectory = Join-Path $repoRoot 'build-x64\bin'
$launcher = Join-Path $binDirectory 'atb_dev_launcher.exe'
$plugin = Join-Path $binDirectory 'AmnesiaTheBunker.FPSFix.asi'
$pluginIni = Join-Path $binDirectory 'AmnesiaTheBunker.FPSFix.ini'
$telemetryIni = Join-Path $repoRoot 'config\AmnesiaTheBunker.FPSFix.telemetry.ini'
$renderUnlockIni = Join-Path $repoRoot 'config\AmnesiaTheBunker.FPSFix.render-unlock.ini'
$limited240Ini = Join-Path $repoRoot 'config\AmnesiaTheBunker.FPSFix.limited-240.ini'
$cameraTraceIni = Join-Path $repoRoot 'config\AmnesiaTheBunker.FPSFix.camera-trace.ini'
$cameraRestoreIni = Join-Path $repoRoot 'config\AmnesiaTheBunker.FPSFix.camera-restore-probe.ini'
$cameraInterpolationIni = Join-Path $repoRoot 'config\AmnesiaTheBunker.FPSFix.camera-interpolation.ini'
$handsTraceIni = Join-Path $repoRoot 'config\AmnesiaTheBunker.FPSFix.hands-trace.ini'
$handsRenderCopyIni = Join-Path $repoRoot 'config\AmnesiaTheBunker.FPSFix.hands-render-copy-probe.ini'
$handsInterpolationIni = Join-Path $repoRoot 'config\AmnesiaTheBunker.FPSFix.hands-interpolation.ini'
$handsPoseTraceIni = Join-Path $repoRoot 'config\AmnesiaTheBunker.FPSFix.hands-pose-trace.ini'
$handsPoseRestoreIni = Join-Path $repoRoot 'config\AmnesiaTheBunker.FPSFix.hands-pose-restore-probe.ini'
$handsPoseRenderCopyIni = Join-Path $repoRoot 'config\AmnesiaTheBunker.FPSFix.hands-pose-render-copy-probe.ini'
$handsPoseInterpolationIni = Join-Path $repoRoot 'config\AmnesiaTheBunker.FPSFix.hands-pose-interpolation.ini'
$handsRestoreIni = Join-Path $repoRoot 'config\AmnesiaTheBunker.FPSFix.hands-restore-probe.ini'
$worldTraceIni = Join-Path $repoRoot 'config\AmnesiaTheBunker.FPSFix.world-trace.ini'
$worldInterpolationIni = Join-Path $repoRoot 'config\AmnesiaTheBunker.FPSFix.world-interpolation.ini'
$playIni = Join-Path $repoRoot 'config\AmnesiaTheBunker.FPSFix.play.ini'

$selectedIni = switch ($Mode) {
    'PluginUnlock' { $renderUnlockIni }
    'PluginLimiter' { $limited240Ini }
    'ClockTelemetry' { $limited240Ini }
    'CameraTrace' { $cameraTraceIni }
    'CameraRestore' { $cameraRestoreIni }
    'CameraInterpolation' { $cameraInterpolationIni }
    'HandsTrace' { $handsTraceIni }
    'HandsRenderCopy' { $handsRenderCopyIni }
    'HandsInterpolation' { $handsInterpolationIni }
    'HandsPoseTrace' { $handsPoseTraceIni }
    'HandsPoseRestore' { $handsPoseRestoreIni }
    'HandsPoseRenderCopy' { $handsPoseRenderCopyIni }
    'HandsPoseInterpolation' { $handsPoseInterpolationIni }
    'HandsRestore' { $handsRestoreIni }
    'WorldTrace' { $worldTraceIni }
    'WorldInterpolation' { $worldInterpolationIni }
    'Play' { $playIni }
    default { $telemetryIni }
}
if ($PluginProfile) {
    if ($Mode -ne 'Play') {
        throw '-PluginProfile is only supported with -Mode Play.'
    }
    $selectedIni = Join-Path $repoRoot "config\AmnesiaTheBunker.FPSFix.$PluginProfile.ini"
}
foreach ($required in @($gameExe, $launcher, $plugin, $selectedIni)) {
    if (-not [System.IO.File]::Exists($required)) {
        throw "Required file does not exist: $required"
    }
}
if (Get-Process -Name 'AmnesiaTheBunker' -ErrorAction SilentlyContinue) {
    throw 'AmnesiaTheBunker.exe is already running. Close it and run this command again.'
}

$backupDirectory = Join-Path $repoRoot 'research\validation\profile-backups'
[System.IO.Directory]::CreateDirectory($backupDirectory) | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$settingsBackup = Join-Path $backupDirectory "user_settings-$stamp.cfg"
[System.IO.File]::WriteAllBytes($settingsBackup, $originalBytes)

$iniExisted = [System.IO.File]::Exists($pluginIni)
$originalIniBytes = if ($iniExisted) {
    [System.IO.File]::ReadAllBytes($pluginIni)
} else {
    $null
}

try {
    Copy-Item -LiteralPath $selectedIni -Destination $pluginIni -Force

    Write-Utf8NoBom $SettingsPath $stockProfile
    if ($Mode -eq 'PluginUnlock') {
        Write-Host ''
        Write-Host 'PLUGIN PROOF: opt-in RenderUnlock' -ForegroundColor Cyan
        Write-Host 'Game profile: RefreshRate=240, GameplayUpdateRate=120, LimitFPS=true, Vsync=false'
        Write-Host 'Plugin profile: Telemetry=true, RenderUnlock=true'
        Write-Host 'This short case may run uncapped. Do not open the in-game Display menu.'
        Write-Host 'After launch: spend about 10 seconds in the menu and 20 seconds in gameplay, then exit normally.'
        Read-Host 'Press Enter to launch the plugin proof'
        & $launcher $gameExe $plugin --wait
        if ($LASTEXITCODE -ne 0) {
            throw "Plugin-proof launcher failed with exit code $LASTEXITCODE."
        }
    } elseif ($Mode -eq 'PluginLimiter') {
        Write-Host ''
        Write-Host 'PLUGIN PROOF: independent 240 FPS presentation limiter' -ForegroundColor Cyan
        Write-Host 'Game profile: RefreshRate=240, GameplayUpdateRate=120, LimitFPS=true, Vsync=false'
        Write-Host 'Plugin profile: Telemetry=true, RenderUnlock=true, PresentationLimiter=true, TargetFPS=240'
        Write-Host 'RTSS may remain enabled; leave its limiter disabled. Do not open the in-game Display menu.'
        Write-Host 'After launch: spend about 10 seconds in the menu and 20 seconds in gameplay, then exit normally.'
        Read-Host 'Press Enter to launch the limiter proof'
        & $launcher $gameExe $plugin --wait
        if ($LASTEXITCODE -ne 0) {
            throw "Limiter-proof launcher failed with exit code $LASTEXITCODE."
        }
    } elseif ($Mode -eq 'ClockTelemetry') {
        Write-Host ''
        Write-Host 'TELEMETRY PROOF: completed update boundary and scheduler phase' -ForegroundColor Cyan
        Write-Host 'Game profile: RefreshRate=240, GameplayUpdateRate=120, LimitFPS=true, Vsync=false'
        Write-Host 'Plugin profile: proven 240 FPS limiter plus read-only clock telemetry'
        Write-Host 'RTSS may remain enabled; leave its limiter disabled. Do not open the in-game Display menu.'
        Write-Host 'After launch: spend about 10 seconds in the menu and 10 seconds in gameplay, then exit normally.'
        Read-Host 'Press Enter to launch the clock-telemetry proof'
        & $launcher $gameExe $plugin --wait
        if ($LASTEXITCODE -ne 0) {
            throw "Clock-telemetry launcher failed with exit code $LASTEXITCODE."
        }
    } elseif ($Mode -eq 'CameraTrace') {
        Write-Host ''
        Write-Host 'READ-ONLY PROOF: current-listener camera identity and motion cadence' -ForegroundColor Cyan
        Write-Host 'Game profile: RefreshRate=240, GameplayUpdateRate=120, LimitFPS=true, Vsync=false'
        Write-Host 'Plugin profile: proven 240 FPS limiter plus read-only camera motion trace'
        Write-Host 'RTSS may remain enabled; leave its limiter disabled. Do not open the in-game Display menu.'
        Write-Host 'After launch: spend about 5 seconds in the menu, then 20 seconds in gameplay.'
        Write-Host 'During gameplay, walk and look around continuously for most of the test, then exit normally.'
        Read-Host 'Press Enter to launch the camera-trace proof'
        & $launcher $gameExe $plugin --wait
        if ($LASTEXITCODE -ne 0) {
            throw "Camera-trace launcher failed with exit code $LASTEXITCODE."
        }
    } elseif ($Mode -eq 'CameraRestore') {
        Write-Host ''
        Write-Host 'WRITE-PATH PROOF: identical camera pose and byte-exact cache restoration' -ForegroundColor Cyan
        Write-Host 'Game profile: RefreshRate=240, GameplayUpdateRate=120, LimitFPS=true, Vsync=false'
        Write-Host 'Plugin profile: proven 240 FPS limiter plus render-scoped camera restore probe'
        Write-Host 'The probe does not interpolate yet; it reapplies the current pose unchanged.'
        Write-Host 'RTSS may remain enabled; leave its limiter disabled. Do not open the in-game Display menu.'
        Write-Host 'After launch: spend about 5 seconds in the menu, then 20 seconds in gameplay.'
        Write-Host 'During gameplay, walk and look around continuously, then exit normally.'
        Read-Host 'Press Enter to launch the camera restore proof'
        & $launcher $gameExe $plugin --wait
        if ($LASTEXITCODE -ne 0) {
            throw "Camera-restore launcher failed with exit code $LASTEXITCODE."
        }
    } elseif ($Mode -eq 'CameraInterpolation') {
        Write-Host ''
        Write-Host 'VISUAL PROOF: main current-listener camera interpolation' -ForegroundColor Cyan
        Write-Host 'Game profile: RefreshRate=240, GameplayUpdateRate=120, LimitFPS=true, Vsync=false'
        Write-Host 'Plugin profile: proven 240 FPS limiter plus camera interpolation'
        Write-Host 'RTSS may remain enabled; leave its limiter disabled. Do not open the in-game Display menu.'
        Write-Host 'After launch: spend about 5 seconds in the menu, then 25 seconds in ordinary gameplay.'
        Write-Host 'Walk and look around continuously. For this first proof, avoid ladders, cutscenes, and special interactions.'
        Write-Host 'The world may still step at 120 Hz; camera turning and translation should look smoother.'
        Read-Host 'Press Enter to launch the camera interpolation proof'
        & $launcher $gameExe $plugin --wait
        if ($LASTEXITCODE -ne 0) {
            throw "Camera-interpolation launcher failed with exit code $LASTEXITCODE."
        }
    } elseif ($Mode -eq 'HandsTrace') {
        Write-Host ''
        Write-Host 'READ-ONLY PROOF: live renderer ownership for hands and equipment' -ForegroundColor Cyan
        Write-Host 'Game profile: RefreshRate=240, GameplayUpdateRate=120, LimitFPS=true, Vsync=false'
        Write-Host 'Plugin profile: proven camera interpolation plus pass-through submesh matrix trace'
        Write-Host 'This probe returns every original matrix pointer unchanged and performs no entity writes.'
        Write-Host 'RTSS may remain enabled; leave its limiter disabled. Do not open the in-game Display menu.'
        Write-Host 'After launch: load into gameplay, then play for about 25 seconds.'
        Write-Host 'Walk/look empty-handed, then draw and move with the gun or lantern.'
        Write-Host 'The level-load transition is part of the safety test; gameplay measures renderer-local transforms.'
        Write-Host 'Keep one gun or lantern drawn and move/look continuously for at least 15 seconds.'
        Read-Host 'Press Enter to launch the read-only renderer trace'
        & $launcher $gameExe $plugin --wait
        if ($LASTEXITCODE -ne 0) {
            throw "Hands-trace launcher failed with exit code $LASTEXITCODE."
        }
    } elseif ($Mode -eq 'HandsRenderCopy') {
        Write-Host ''
        Write-Host 'RETURN-PATH PROOF: byte-exact first-person renderer matrix copy' -ForegroundColor Cyan
        Write-Host 'Game profile: RefreshRate=240, GameplayUpdateRate=120, LimitFPS=true, Vsync=false'
        Write-Host 'Plugin profile: proven camera interpolation plus near-mesh render-copy probe'
        Write-Host 'The returned matrix bytes are unchanged; no game object or cache is modified.'
        Write-Host 'RTSS may remain enabled; leave its limiter disabled. Do not open the in-game Display menu.'
        Write-Host 'After launch: load into gameplay, draw the gun or lantern, and play for about 20 seconds.'
        Write-Host 'Move/look continuously and exercise ordinary draw/holster once if convenient.'
        Read-Host 'Press Enter to launch the renderer-copy proof'
        & $launcher $gameExe $plugin --wait
        if ($LASTEXITCODE -ne 0) {
            throw "Hands-render-copy launcher failed with exit code $LASTEXITCODE."
        }
    } elseif ($Mode -eq 'HandsInterpolation') {
        Write-Host ''
        Write-Host 'VISUAL PROOF: first-person root interpolation through renderer return path' -ForegroundColor Cyan
        Write-Host 'Game profile: RefreshRate=240, GameplayUpdateRate=120, LimitFPS=true, Vsync=false'
        Write-Host 'Plugin profile: proven camera interpolation plus near-mesh root interpolation'
        Write-Host 'No game object or matrix cache is written; ineligible frames return the original pointer.'
        Write-Host 'RTSS may remain enabled; leave its limiter disabled. Do not open the in-game Display menu.'
        Write-Host 'After launch: load into ordinary gameplay, draw the gun or lantern, and play for about 25 seconds.'
        Write-Host 'Move and look continuously. Avoid ladders, cutscenes, and special interactions for this first proof.'
        Write-Host 'Compare the held item against the still-stepped player body and loose world objects.'
        Read-Host 'Press Enter to launch first-person interpolation'
        & $launcher $gameExe $plugin --wait
        if ($LASTEXITCODE -ne 0) {
            throw "Hands-interpolation launcher failed with exit code $LASTEXITCODE."
        }
    } elseif ($Mode -eq 'HandsPoseTrace') {
        Write-Host ''
        Write-Host 'READ-ONLY PROOF: live first-person final skin-palette cadence' -ForegroundColor Cyan
        Write-Host 'Game profile: RefreshRate=240, GameplayUpdateRate=120, LimitFPS=true, Vsync=false'
        Write-Host 'Plugin profile: proven camera/gun interpolation plus pass-through renderer-palette trace'
        Write-Host 'The renderer runs unchanged; its final dual-quaternion palette pointer is returned unmodified.'
        Write-Host 'Both matrix and renderer palettes are only validated and hashed.'
        Write-Host 'RTSS may remain enabled; leave its limiter disabled. Do not open the in-game Display menu.'
        Write-Host 'After launch: load into ordinary gameplay and play for about 25 seconds.'
        Write-Host 'Keep the gun or lantern drawn; walk, look, aim, and reload or interact once if convenient.'
        Write-Host 'The level-load transition is part of this renderer-lifetime safety test.'
        Read-Host 'Press Enter to launch the read-only renderer-palette trace'
        & $launcher $gameExe $plugin --wait
        if ($LASTEXITCODE -ne 0) {
            throw "Hands-pose-trace launcher failed with exit code $LASTEXITCODE."
        }
    } elseif ($Mode -eq 'HandsPoseRestore') {
        Write-Host ''
        Write-Host 'WRITE-PATH PROOF: byte-identical 23-bone first-person palette restore' -ForegroundColor Cyan
        Write-Host 'Game profile: RefreshRate=240, GameplayUpdateRate=120, LimitFPS=true, Vsync=false'
        Write-Host 'Plugin profile: proven camera/gun interpolation plus render-call-scoped palette restore'
        Write-Host 'No interpolation yet: the current palette is reapplied unchanged, restored, and verified.'
        Write-Host 'RTSS may remain enabled; leave its limiter disabled. Do not open the in-game Display menu.'
        Write-Host 'After launch: load into ordinary gameplay and play for about 25 seconds.'
        Write-Host 'Keep the gun or lantern drawn; walk, look, aim, and reload or interact once if convenient.'
        Write-Host 'The level-load transition is part of this renderer-lifetime safety test.'
        Read-Host 'Press Enter to launch the hands-palette restore proof'
        & $launcher $gameExe $plugin --wait
        if ($LASTEXITCODE -ne 0) {
            throw "Hands-pose-restore launcher failed with exit code $LASTEXITCODE."
        }
    } elseif ($Mode -eq 'HandsPoseRenderCopy') {
        Write-Host ''
        Write-Host 'RETURN-PATH PROOF: byte-identical 23-bone renderer palette copy' -ForegroundColor Cyan
        Write-Host 'Game profile: RefreshRate=240, GameplayUpdateRate=120, LimitFPS=true, Vsync=false'
        Write-Host 'Plugin profile: proven camera/gun interpolation plus renderer-palette copy'
        Write-Host 'Only the near 23-bone dual-quaternion array is copied; no game memory is written.'
        Write-Host 'RTSS may remain enabled; leave its limiter disabled. Do not open the in-game Display menu.'
        Write-Host 'After launch: load into ordinary gameplay and play for about 25 seconds.'
        Write-Host 'Keep the gun drawn; walk, look, aim, and reload once if convenient.'
        Write-Host 'The level-load transition and draw/holster lifetime are part of this safety test.'
        Read-Host 'Press Enter to launch the renderer-palette copy proof'
        & $launcher $gameExe $plugin --wait
        if ($LASTEXITCODE -ne 0) {
            throw "Hands-pose-render-copy launcher failed with exit code $LASTEXITCODE."
        }
    } elseif ($Mode -eq 'HandsPoseInterpolation') {
        Write-Host ''
        Write-Host 'VISUAL PROOF: first-person hands and body pose interpolation' -ForegroundColor Cyan
        Write-Host 'Game profile: RefreshRate=240, GameplayUpdateRate=120, LimitFPS=true, Vsync=false'
        Write-Host 'Plugin profile: proven camera/gun root plus final hands-palette interpolation'
        Write-Host 'The held-item rig and the player-body (arms/hands) palettes and roots are returned from plugin-owned storage.'
        Write-Host 'No game object or palette memory is written; all other bone counts keep the original pointer.'
        Write-Host 'RTSS may remain enabled; leave its limiter disabled. Do not open the in-game Display menu.'
        Write-Host 'After launch: load into ordinary gameplay and play for about 30 seconds.'
        Write-Host 'Keep the gun drawn; walk, look, aim, and reload while watching the visible hands.'
        Write-Host 'Avoid ladders, cutscenes, and special interactions for this first visual proof.'
        Read-Host 'Press Enter to launch hands-pose interpolation'
        & $launcher $gameExe $plugin --wait
        if ($LASTEXITCODE -ne 0) {
            throw "Hands-pose-interpolation launcher failed with exit code $LASTEXITCODE."
        }
    } elseif ($Mode -eq 'Play') {
        Write-Host ''
        if ($PluginProfile -eq 'baseline') {
            Write-Host 'BASELINE MEASUREMENT: frame-rate unlock and limiter only, 60 Hz simulation' -ForegroundColor Cyan
            Write-Host 'No interpolation: the view WILL look stepped. This run only measures what a tick costs without the plugin''s layers.'
            Write-Host 'Load the same save, go to the same spot as in the play run, and move and look around there for about 30 seconds.'
            Write-Host 'RTSS may remain enabled; leave its limiter disabled. Do not open the in-game Display menu.'
        } else {
            Write-Host 'PLAY SESSION: all validated layers, evenly paced up to 350 FPS over a 60 Hz simulation' -ForegroundColor Cyan
            Write-Host 'Camera, hands, body, held items, world objects, enemy animation and shadows are interpolated.'
            Write-Host 'Read-only traces stay on and record what the game shows, including skinned world meshes.'
            Write-Host 'RTSS may remain enabled; leave its limiter disabled. Do not open the in-game Display menu.'
            Write-Host 'Play normally for as long as you like. Note the time of anything that looks stepped or wrong.'
        }
        Read-Host 'Press Enter to launch'
        & $launcher $gameExe $plugin --wait
        if ($LASTEXITCODE -ne 0) {
            throw "Play-session launcher failed with exit code $LASTEXITCODE."
        }
        # Keep this run's frame trace under its profile's name; the next run
        # overwrites the live file.
        $frameTrace = Join-Path $binDirectory 'AmnesiaTheBunker.FPSFix.frames.bin'
        if ([System.IO.File]::Exists($frameTrace)) {
            $frameLabel = if ($PluginProfile) { $PluginProfile } else { 'play' }
            Copy-Item -LiteralPath $frameTrace -Destination (Join-Path $binDirectory "AmnesiaTheBunker.FPSFix.frames.$frameLabel.bin") -Force
        }
    } elseif ($Mode -eq 'WorldTrace' -or $Mode -eq 'WorldInterpolation') {
        Write-Host ''
        if ($Mode -eq 'WorldTrace') {
            Write-Host 'READ-ONLY TRACE: world-object model matrices' -ForegroundColor Cyan
            Write-Host 'Plugin profile: proven camera/hands/body interpolation plus a pass-through world-object trace'
            Write-Host 'World objects are expected to remain stepped in this run; nothing is substituted for them.'
        } else {
            Write-Host 'VISUAL PROOF: world-object root interpolation' -ForegroundColor Cyan
            Write-Host 'Plugin profile: proven camera/hands/body interpolation plus interpolated world-object roots'
            Write-Host 'Moving world objects receive an interpolated matrix from plugin-owned storage.'
            Write-Host 'This profile also holds the simulation at 60 Hz (GameplayRateOverride=60) under 240 FPS presentation.'
        }
        Write-Host 'Game profile: RefreshRate=240, GameplayUpdateRate=120, LimitFPS=true, Vsync=false'
        Write-Host 'RTSS may remain enabled; leave its limiter disabled. Do not open the in-game Display menu.'
        Write-Host 'After launch: load into ordinary gameplay and play for about a minute.'
        Write-Host 'Pick up, carry, and throw a few loose objects; open and close a door and a drawer.'
        Read-Host 'Press Enter to launch'
        & $launcher $gameExe $plugin --wait
        if ($LASTEXITCODE -ne 0) {
            throw "World-object launcher failed with exit code $LASTEXITCODE."
        }
    } elseif ($Mode -eq 'HandsRestore') {
        Write-Host ''
        Write-Host 'WRITE-PATH PROOF: identical hands/equipment matrices and exact restoration' -ForegroundColor Cyan
        Write-Host 'Game profile: RefreshRate=240, GameplayUpdateRate=120, LimitFPS=true, Vsync=false'
        Write-Host 'Plugin profile: proven camera interpolation plus render-scoped entity restore probe'
        Write-Host 'The probe does not interpolate entities yet; it reapplies each current matrix unchanged.'
        Write-Host 'RTSS may remain enabled; leave its limiter disabled. Do not open the in-game Display menu.'
        Write-Host 'After launch: spend about 5 seconds in the menu, then about 20 seconds in ordinary gameplay.'
        Write-Host 'Walk/look empty-handed, then draw and move with the gun or lantern.'
        Read-Host 'Press Enter to launch the hands/equipment restore proof'
        & $launcher $gameExe $plugin --wait
        if ($LASTEXITCODE -ne 0) {
            throw "Hands-restore launcher failed with exit code $LASTEXITCODE."
        }
    } else {
        Write-Host ''
        Write-Host 'CASE 1/2: stock render gate' -ForegroundColor Cyan
        Write-Host 'RefreshRate=240, GameplayUpdateRate=120, LimitFPS=true, Vsync=false'
        Write-Host 'RTSS may remain enabled; leave its limiter disabled.'
        Write-Host 'After launch: spend about 10 seconds in the menu and 20 seconds in gameplay, then exit normally.'
        Read-Host 'Press Enter to launch case 1'
        & $launcher $gameExe $plugin --wait
        if ($LASTEXITCODE -ne 0) {
            throw "Case 1 launcher failed with exit code $LASTEXITCODE."
        }

        if (Get-Process -Name 'AmnesiaTheBunker' -ErrorAction SilentlyContinue) {
            throw 'The first game process is still running; refusing to edit its profile.'
        }
        Write-Utf8NoBom $SettingsPath $unlockedProfile
        Write-Host ''
        Write-Host 'CASE 2/2: existing engine unlock path' -ForegroundColor Cyan
        Write-Host 'RefreshRate=240, GameplayUpdateRate=120, LimitFPS=false, Vsync=false'
        Write-Host 'This short case may run uncapped. Do not open the in-game Display menu.'
        Write-Host 'After launch: spend about 10 seconds in the menu and 20 seconds in gameplay, then exit normally.'
        Read-Host 'Press Enter to launch case 2'
        & $launcher $gameExe $plugin --wait
        if ($LASTEXITCODE -ne 0) {
            throw "Case 2 launcher failed with exit code $LASTEXITCODE."
        }
    }
}
finally {
    if ($KeepTestSettings) {
        Write-Host ''
        Write-Host 'Test game settings and plugin INI left staged for the next run.' -ForegroundColor Yellow
        Write-Host "Pre-test safety backup retained at: $settingsBackup"
    } else {
        [System.IO.File]::WriteAllBytes($SettingsPath, $originalBytes)
        if ($iniExisted) {
            [System.IO.File]::WriteAllBytes($pluginIni, $originalIniBytes)
        } elseif ([System.IO.File]::Exists($pluginIni)) {
            Remove-Item -LiteralPath $pluginIni -Force
        }
        $restoredBytes = [System.IO.File]::ReadAllBytes($SettingsPath)
        if ([Convert]::ToBase64String($originalBytes) -ne
            [Convert]::ToBase64String($restoredBytes)) {
            throw "Automatic profile restoration verification failed. Backup: $settingsBackup"
        }
        Write-Host ''
        Write-Host 'Original user settings restored byte-for-byte.' -ForegroundColor Green
        Write-Host "Safety backup retained at: $settingsBackup"
    }
}

if ($Mode -eq 'PluginUnlock') {
    Write-Host 'Plugin RenderUnlock proof completed. Tell Codex the test is finished.' -ForegroundColor Green
} elseif ($Mode -eq 'PluginLimiter') {
    Write-Host 'Plugin PresentationLimiter proof completed. Tell Codex the test is finished.' -ForegroundColor Green
} elseif ($Mode -eq 'ClockTelemetry') {
    Write-Host 'Shared-clock telemetry proof completed. Tell Codex the test is finished.' -ForegroundColor Green
} elseif ($Mode -eq 'CameraTrace') {
    Write-Host 'Read-only camera trace proof completed. Tell Codex the test is finished.' -ForegroundColor Green
} elseif ($Mode -eq 'CameraRestore') {
    Write-Host 'Camera restore-integrity proof completed. Tell Codex the test is finished.' -ForegroundColor Green
} elseif ($Mode -eq 'CameraInterpolation') {
    Write-Host 'Camera interpolation proof completed. Tell Codex the test is finished.' -ForegroundColor Green
} elseif ($Mode -eq 'HandsTrace') {
    Write-Host 'Read-only hands/equipment renderer trace completed. Tell Codex the test is finished.' -ForegroundColor Green
} elseif ($Mode -eq 'HandsRenderCopy') {
    Write-Host 'First-person renderer-copy proof completed. Tell Codex the test is finished.' -ForegroundColor Green
} elseif ($Mode -eq 'HandsInterpolation') {
    Write-Host 'First-person root interpolation proof completed. Tell Codex the test is finished.' -ForegroundColor Green
} elseif ($Mode -eq 'HandsPoseTrace') {
    Write-Host 'Read-only first-person skin-palette trace completed. Tell Codex the test is finished.' -ForegroundColor Green
} elseif ($Mode -eq 'HandsPoseRestore') {
    Write-Host 'First-person skin-palette restore proof completed. Tell Codex the test is finished.' -ForegroundColor Green
} elseif ($Mode -eq 'HandsPoseRenderCopy') {
    Write-Host 'First-person renderer-palette copy proof completed. Tell Codex the test is finished.' -ForegroundColor Green
} elseif ($Mode -eq 'HandsPoseInterpolation') {
    Write-Host 'First-person hands-pose interpolation proof completed. Tell Codex the test is finished.' -ForegroundColor Green
} elseif ($Mode -eq 'Play') {
    Write-Host 'Play session completed.' -ForegroundColor Green
} elseif ($Mode -eq 'WorldTrace' -or $Mode -eq 'WorldInterpolation') {
    Write-Host 'World-object run completed.' -ForegroundColor Green
} elseif ($Mode -eq 'HandsRestore') {
    Write-Host 'Hands/equipment restore-integrity proof completed. Tell Codex the test is finished.' -ForegroundColor Green
} else {
    Write-Host 'Both validation runs completed. Tell Codex the tests are finished.' -ForegroundColor Green
}
