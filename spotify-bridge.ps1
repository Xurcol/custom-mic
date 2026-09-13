# Custom Mic - Spotify bridge
#
# Follows the Spotify desktop app through Windows' media session API
# (GlobalSystemMediaTransportControls), so there is no login and free accounts
# work. Reports state as one JSON object per line on stdout; reads commands,
# one per line, on stdin:
#   toggle | play | pause | next | prev | seek <ms> | shuffle <true|false>
# Exits when stdin closes, so it never outlives the app that started it.

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
Add-Type -AssemblyName System.Runtime.WindowsRuntime

$asTaskOp = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
    $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'
})[0]

function Await($op, [Type]$type) {
    $task = $asTaskOp.MakeGenericMethod($type).Invoke($null, @($op))
    if (-not $task.Wait(4000)) { throw 'Windows media call timed out' }
    return $task.Result
}

$null = [Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager, Windows.Media.Control, ContentType = WindowsRuntime]
$null = [Windows.Storage.Streams.IRandomAccessStreamWithContentType, Windows.Storage.Streams, ContentType = WindowsRuntime]
$null = [Windows.Storage.Streams.IInputStream, Windows.Storage.Streams, ContentType = WindowsRuntime]
$null = [Windows.Storage.Streams.IContentTypeProvider, Windows.Storage.Streams, ContentType = WindowsRuntime]

$manager = Await ([Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager]::RequestAsync()) ([Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager])

function Emit($obj) {
    [Console]::Out.WriteLine(($obj | ConvertTo-Json -Compress -Depth 4))
    [Console]::Out.Flush()
}

function Get-SpotifySession {
    foreach ($s in $manager.GetSessions()) {
        if ($s.SourceAppUserModelId -match 'spotify') { return $s }
    }
    return $null
}

# Cover art is only read when the track changes: it is the expensive part.
#
# PowerShell 5.1 sees the thumbnail stream as a bare System.__ComObject: its
# Size and ContentType read as empty, its methods are invisible, and passing
# it to AsStreamForRead fails overload binding. Invoking through the
# interfaces by reflection reaches the real object.
$asStreamForRead = [System.IO.WindowsRuntimeStreamExtensions].GetMethod('AsStreamForRead', [Type[]]@([Windows.Storage.Streams.IInputStream]))
$contentTypeProperty = [Windows.Storage.Streams.IContentTypeProvider].GetProperty('ContentType')

function Read-Cover($props) {
    try {
        if (-not $props.Thumbnail) { return '' }
        $ras = Await ($props.Thumbnail.OpenReadAsync()) ([Windows.Storage.Streams.IRandomAccessStreamWithContentType])
        $stream = $asStreamForRead.Invoke($null, @($ras))
        $buffer = New-Object System.IO.MemoryStream
        $stream.CopyTo($buffer)
        $stream.Dispose()
        $type = $null
        try { $type = $contentTypeProperty.GetValue($ras) } catch { }
        if (-not $type) { $type = 'image/jpeg' }
        return 'data:' + $type + ';base64,' + [Convert]::ToBase64String($buffer.ToArray())
    } catch {
        return ''
    }
}

# Console.In.ReadLineAsync blocks on .NET Framework, which would freeze the
# polling loop until a command arrived. A StreamReader over the raw stdin
# stream reads on a worker thread instead.
$stdinReader = New-Object System.IO.StreamReader([Console]::OpenStandardInput(), (New-Object System.Text.UTF8Encoding($false)))
$pendingLine = $stdinReader.ReadLineAsync()

$lastKey = ''
$lastTrack = ''
$lastEmit = [DateTime]::MinValue

while ($true) {
    while ($pendingLine.IsCompleted) {
        $line = $null
        try { $line = $pendingLine.Result } catch { exit 0 }
        if ($null -eq $line) { exit 0 }
        $pendingLine = $stdinReader.ReadLineAsync()

        $parts = $line.Trim().Split(' ')
        $session = Get-SpotifySession
        if (-not $session) { continue }
        try {
            switch ($parts[0]) {
                'toggle'  { $null = Await ($session.TryTogglePlayPauseAsync()) ([bool]) }
                'play'    { $null = Await ($session.TryPlayAsync()) ([bool]) }
                'pause'   { $null = Await ($session.TryPauseAsync()) ([bool]) }
                'next'    { $null = Await ($session.TrySkipNextAsync()) ([bool]) }
                'prev'    { $null = Await ($session.TrySkipPreviousAsync()) ([bool]) }
                'seek'    { $null = Await ($session.TryChangePlaybackPositionAsync([long]([double]$parts[1] * 10000))) ([bool]) }
                'shuffle' { $null = Await ($session.TryChangeShuffleActiveAsync($parts[1] -eq 'true')) ([bool]) }
            }
        } catch {
            Emit @{ type = 'error'; message = ('command ' + $parts[0] + ': ' + $_.Exception.Message) }
        }
        # Report the outcome now rather than on the next change.
        $lastKey = ''
    }

    try {
        $session = Get-SpotifySession
        if (-not $session) {
            if ($lastKey -ne 'none') {
                Emit @{ type = 'state'; available = $false }
                $lastKey = 'none'
                $lastTrack = ''
            }
        } else {
            $props = Await ($session.TryGetMediaPropertiesAsync()) ([Windows.Media.Control.GlobalSystemMediaTransportControlsSessionMediaProperties])
            $info = $session.GetPlaybackInfo()
            $timeline = $session.GetTimelineProperties()

            $status = [string]$info.PlaybackStatus
            $positionMs = [long]$timeline.Position.TotalMilliseconds
            $endMs = [long]$timeline.EndTime.TotalMilliseconds
            $updatedAt = 0
            try { $updatedAt = $timeline.LastUpdatedTime.ToUnixTimeMilliseconds() } catch { }
            $shuffle = $false
            try { if ($info.IsShuffleActive) { $shuffle = $true } } catch { }

            $track = [string]$props.Title + '|' + [string]$props.Artist + '|' + [string]$props.AlbumTitle
            $key = $track + '|' + $status + '|' + $positionMs + '|' + $endMs + '|' + $shuffle
            $now = Get-Date

            if ($key -ne $lastKey -or ($now - $lastEmit).TotalSeconds -ge 5) {
                $state = [ordered]@{
                    type              = 'state'
                    available         = $true
                    title             = [string]$props.Title
                    artist            = [string]$props.Artist
                    album             = [string]$props.AlbumTitle
                    status            = $status
                    positionMs        = $positionMs
                    endMs             = $endMs
                    timelineUpdatedAt = $updatedAt
                    shuffle           = $shuffle
                    canNext           = [bool]$info.Controls.IsNextEnabled
                    canPrev           = [bool]$info.Controls.IsPreviousEnabled
                    trackKey          = $track
                }
                if ($track -ne $lastTrack) {
                    $state.cover = Read-Cover $props
                    $lastTrack = $track
                }
                Emit $state
                $lastKey = $key
                $lastEmit = $now
            }
        }
    } catch {
        Emit @{ type = 'error'; message = $_.Exception.Message }
        Start-Sleep -Milliseconds 1500
    }

    Start-Sleep -Milliseconds 400
}
