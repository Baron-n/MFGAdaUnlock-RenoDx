# SPDX-License-Identifier: MIT
# Standalone assisted repair. Dot-sourcing exposes transaction functions for
# tests; the interactive entry point never offers an unattended confirmation.
[CmdletBinding()]
param([string]$GameDirectory,[int]$GameProcessId,[long]$GameCreationTicks,
      [string]$GameExecutable,[string]$CatalogPath,[string]$RollbackJournal)
$ErrorActionPreference='Stop'
function Get-XeDigest([string]$Path) {
  if(!(Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
  return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Read-XeCatalog([string]$Path) {
  $c=Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json
  $base='https://raw.githubusercontent.com/intel/xess/de0fb9c1c510661c571164e1418ceca8101dab69/'
  $pins=@{
    'libxess_fg.dll'=@('bin/libxess_fg.dll','ec5e0c65e075570c6ede72618bb666d0be0c2e10b2ea9762c0fe8cb8e375ab27')
    'libxell.dll'=@('bin/libxell.dll','d2030dcd694fda8f2ec7e044b13e6db8f0b56d4ba9113a5efad334e3f3ded8c7')
    'Intel-XeSS-LICENSE.txt'=@('LICENSE.txt','784be42c39a4a4d03cf85d88e15e59d75828fca773ce45b8dbfffbd7eaf208df')
    'Intel-XeSS-third-party-programs.txt'=@('third-party-programs.txt','e8d8899dbfc4de0e8ca25560be9358e03c1405d7c956d20a1838c3c709b9b51c')
  }
  if($c.schema -ne 1 -or $c.base_url -cne $base -or @($c.files).Count -ne 4) { throw 'Untrusted dependency catalog.' }
  $seen=@{}
  foreach($entry in $c.files) {
    if(!$pins.ContainsKey($entry.name) -or $seen.ContainsKey($entry.name)) { throw 'Unknown or duplicated catalog entry.' }
    $seen[$entry.name]=$true; $pin=$pins[$entry.name]
    if($entry.source -cne $pin[0] -or $entry.sha256 -cne $pin[1]) { throw 'Catalog source/hash differs from the trusted helper.' }
    if([bool]$entry.required -ne $entry.name.EndsWith('.dll')) { throw 'Invalid required-library classification.' }
    if($entry.name -eq 'libxess_fg.dll' -and ($entry.version -cne '1.3.1.78' -or $entry.accepted_older)) { throw 'Unknown frame-generation profile.' }
    if($entry.name -eq 'libxell.dll' -and $entry.version -cne '1.3.2.10') { throw 'Unknown latency version.' }
    if($entry.name -eq 'libxell.dll' -and (@($entry.accepted_older).Count -ne 1 -or $entry.accepted_older[0] -cne '61aeeab7fdcfb6d1742f86c14c68c663d759607d01a6132645c857291eaebb86')) { throw 'Unknown optional runtime profile.' }
  }
  return $c
}
function Assert-XePath([string]$Root,[string]$Path) {
  $base=[IO.Path]::GetFullPath($Root).TrimEnd('\')
  $full=[IO.Path]::GetFullPath($Path)
  if(!$full.StartsWith($base+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Path escapes game directory.' }
  $part=$full
  while($part) {
    if(Test-Path -LiteralPath $part) {
      if((Get-Item -LiteralPath $part -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Reparse-point paths require manual installation.' }
    }
    if($part -eq $base) { break }
    $part=Split-Path -Parent $part
  }
  return $full
}
function Write-XeJournal($Journal,[string]$Path) {
  $temp=$Path+'.writing'
  $json=$Journal | ConvertTo-Json -Depth 12
  $bytes=[Text.Encoding]::UTF8.GetBytes($json)
  $stream=[IO.File]::Open($temp,[IO.FileMode]::Create,[IO.FileAccess]::Write,[IO.FileShare]::None)
  try { $stream.Write($bytes,0,$bytes.Length); $stream.Flush($true) } finally { $stream.Dispose() }
  if(Test-Path -LiteralPath $Path) { [IO.File]::Replace($temp,$Path,[NullString]::Value) } else { [IO.File]::Move($temp,$Path) }
}
function Restore-XeTransaction([string]$JournalPath,[scriptblock]$AssertClosed) {
  & $AssertClosed
  $j=Get-Content -LiteralPath $JournalPath -Raw -Encoding UTF8 | ConvertFrom-Json
  if($j.schema -ne 1) { throw 'Unknown journal schema.' }
  $backupRoot=Split-Path -Parent ([IO.Path]::GetFullPath($JournalPath))
  [void](Assert-XePath $j.root $backupRoot)
  # Validate all targets BEFORE restoring any, including a crash between the
  # atomic replacement and journal advancement. Never overwrite foreign edits.
  foreach($e in $j.files) {
    [void](Assert-XePath $j.root $e.target)
    $current=Get-XeDigest $e.target
    if($current -ne $e.before -and $current -ne $e.after) { throw ('Concurrent change; rollback refused: '+$e.target) }
    if($e.before) {
      [void](Assert-XePath $backupRoot $e.backup)
      if((Get-XeDigest $e.backup) -ne $e.before) { throw 'Backup integrity check failed.' }
    }
  }
  foreach($e in @($j.files)[($j.files.Count-1)..0]) {
    & $AssertClosed
    $current=Get-XeDigest $e.target
    if($current -eq $e.before) { continue }
    if($e.before) {
      $stage=$e.target+'.mfg-restore-'+[guid]::NewGuid().ToString('N')
      try {
        [IO.File]::Copy($e.backup,$stage,$false)
        if((Get-XeDigest $e.target) -ne $e.after) { throw 'Target changed during rollback.' }
        [IO.File]::Replace($stage,$e.target,[NullString]::Value)
      } finally { if(Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Force } }
    } else {
      if((Get-XeDigest $e.target) -ne $e.after) { throw 'New target changed during rollback.' }
      Remove-Item -LiteralPath $e.target
    }
  }
  $j.state='rolled-back'; Write-XeJournal $j $JournalPath
}
function Invoke-XeTransaction([string]$Root,$Plan,[scriptblock]$AssertClosed) {
  if(!$Plan -or !$Plan.Count) { throw 'Nothing to install.' }
  & $AssertClosed
  $rootPath=(Resolve-Path -LiteralPath $Root).Path
  foreach($e in $Plan) {
    [void](Assert-XePath $rootPath $e.target)
    if((Get-XeDigest $e.target) -ne $e.before) { throw 'Target changed after review. Prepare a fresh review.' }
    if((Get-XeDigest $e.stage) -ne $e.after) { throw 'Staged download failed integrity check.' }
  }
  $backup=Join-Path $rootPath ('.mfg-dependency-backups\'+(Get-Date -Format 'yyyyMMdd-HHmmss')+'-'+[guid]::NewGuid().ToString('N'))
  [void](Assert-XePath $rootPath $backup)
  New-Item -ItemType Directory -Path $backup | Out-Null
  $files=@(); $index=0
  foreach($e in $Plan) {
    $stored=Join-Path $backup ([string]$index+'.original'); $index++
    if($e.before) {
      [IO.File]::Copy($e.target,$stored,$false)
      if((Get-XeDigest $stored) -ne $e.before) { throw 'Backup changed during preparation.' }
    }
    $files+= [ordered]@{target=$e.target;before=$e.before;after=$e.after;stage=$e.stage;backup=$stored}
  }
  $j=[ordered]@{schema=1;root=$rootPath;state='prepared';files=$files}
  $journal=Join-Path $backup 'journal.json'; Write-XeJournal $j $journal
  try {
    foreach($e in $files) {
      & $AssertClosed
      [void](Assert-XePath $rootPath $e.target)
      if((Get-XeDigest $e.target) -ne $e.before) { throw 'Target changed before replacement.' }
      $stage=$e.target+'.mfg-stage-'+[guid]::NewGuid().ToString('N')
      try {
        [IO.File]::Copy($e.stage,$stage,$false)
        if((Get-XeDigest $stage) -ne $e.after) { throw 'Stage copy failed verification.' }
        if((Get-XeDigest $e.target) -ne $e.before) { throw 'Target changed while staging.' }
        if($e.before) { [IO.File]::Replace($stage,$e.target,[NullString]::Value) } else { [IO.File]::Move($stage,$e.target) }
        if((Get-XeDigest $e.target) -ne $e.after) { throw 'Installed hash differs.' }
      } finally { if(Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Force } }
      $j.state='applying'; Write-XeJournal $j $journal
    }
    $j.state='complete'; Write-XeJournal $j $journal
  } catch {
    $reason=$_
    try { Restore-XeTransaction $journal $AssertClosed } catch { Write-Warning ('Rollback requires review: '+$journal+'; '+$_) }
    throw $reason
  }
  return $journal
}
function Start-XeRepair {
  Add-Type -AssemblyName System.Windows.Forms
  if(!$GameExecutable -or !$GameDirectory) { throw 'Game identity is required.' }
  $root=(Resolve-Path -LiteralPath $GameDirectory).Path
  if([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($GameExecutable)) -ne $root) { throw 'Game path does not match installation root.' }
  $assertClosed={
    foreach($p in Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($GameExecutable)) -ErrorAction SilentlyContinue) {
      try { $path=$p.Path } catch { throw 'Cannot verify running game identity. Close it and retry.' }
      if(!$path -or $path -eq $GameExecutable) { throw 'Game is running; installation is deferred until it exits.' }
    }
  }.GetNewClosure()
  if($RollbackJournal) {
    if([Windows.Forms.MessageBox]::Show('Restore the verified dependency backup? Game must be closed.','MFG Unlock rollback','YesNo') -eq 'Yes') {
      Restore-XeTransaction $RollbackJournal $assertClosed
    }
    return
  }
  $process=Get-Process -Id $GameProcessId -ErrorAction Stop
  if($process.Path -ne $GameExecutable -or $process.StartTime.ToFileTimeUtc() -ne $GameCreationTicks) { throw 'The game process identity changed.' }
  $catalog=Read-XeCatalog $CatalogPath
  $base='https://raw.githubusercontent.com/intel/xess/de0fb9c1c510661c571164e1418ceca8101dab69/'
  # Pin the review to selected module paths; mapped copies outside these
  # directories require a manual repair rather than guessing an install path.
  foreach($m in $process.Modules) {
    if($m.ModuleName -in @('libxess_fg.dll','libxell.dll')) {
      $parent=[IO.Path]::GetDirectoryName($m.FileName)
      if($parent -ne $root -and $parent -ne (Join-Path $root '_storage_')) { throw 'An Intel DLL is mapped outside the game root/_storage_; repair manually.' }
    }
  }
  $cache=Join-Path ([IO.Path]::GetTempPath()) ('MFG-XeRepair-'+[guid]::NewGuid().ToString('N'))
  New-Item -ItemType Directory -Path $cache | Out-Null
  $plan=@()
  foreach($entry in $catalog.files) {
    $download=Join-Path $cache $entry.name
    if($entry.name -notin @('libxess_fg.dll','libxell.dll','Intel-XeSS-LICENSE.txt','Intel-XeSS-third-party-programs.txt') -or $entry.source -match '\.\.|:|\\') { throw 'Invalid catalog file.' }
    $targets=@((Join-Path $root $entry.name))
    if($entry.required -and (Test-Path -LiteralPath (Join-Path $root ('_storage_\'+$entry.name)))) { $targets+=Join-Path $root ('_storage_\'+$entry.name) }
    $needed=@($targets | Where-Object { $hash=Get-XeDigest $_; $hash -ne $entry.sha256 -and (!$entry.required -or $hash -notin @($entry.accepted_older)) })
    if(!$needed.Count) { continue }
    Write-Host ('Downloading original Intel file: '+$entry.name)
    [Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -UseBasicParsing -Uri ($base+$entry.source) -OutFile $download
    if((Get-XeDigest $download) -ne $entry.sha256) { throw 'Official download did not match the pinned hash.' }
    if($entry.required -and [Diagnostics.FileVersionInfo]::GetVersionInfo($download).FileVersion -ne $entry.version) { throw 'Downloaded runtime version differs from the admitted profile.' }
    foreach($target in $needed) {
      [void](Assert-XePath $root $target)
      $plan+=[ordered]@{target=$target;before=(Get-XeDigest $target);after=$entry.sha256;stage=$download}
    }
  }
  if(!$plan.Count) { [void][Windows.Forms.MessageBox]::Show('The installed Intel files are already admitted. No replacement is needed.','MFG Unlock'); return }
  $review=($plan | ForEach-Object { $_.target+"`r`n  Existing: "+$(if($_.before){$_.before}else{'MISSING'})+"`r`n  Install: "+$_.after }) -join "`r`n`r`n"
  $message="Install only these verified original Intel files? Unknown existing files will be backed up. Settings, overlays and libxess.dll are preserved.`r`n`r`n"+$review+"`r`n`r`nRead LICENSE and notices in: "+$cache+"`r`nAfter confirmation this helper waits for the game to exit. It will never close the game for you."
  if([Windows.Forms.MessageBox]::Show($message,'Review Intel dependency repair','YesNo','Question') -ne 'Yes') { return }
  Write-Host 'Approved. Close the game normally to continue; do not relaunch until this helper finishes.'
  $process.WaitForExit()
  $journal=Invoke-XeTransaction $root $plan $assertClosed
  [void][Windows.Forms.MessageBox]::Show("Repair complete. Restart the game.`r`nRollback journal: "+$journal,'MFG Unlock')
}
if($MyInvocation.InvocationName -ne '.') {
  try { Start-XeRepair } catch {
    Write-Error $_ -ErrorAction Continue
    try { Add-Type -AssemblyName System.Windows.Forms; [void][Windows.Forms.MessageBox]::Show($_.Exception.Message,'MFG Unlock repair stopped') } catch {}
    exit 1
  }
}

