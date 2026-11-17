# Runs the check on a Windows host. Usage: run.ps1 <dir with sgi-gpu-win-check.exe>
param([string]$Dir = 'win-stage')
$ErrorActionPreference = 'Continue'
$exe = Join-Path (Resolve-Path $Dir) 'sgi-gpu-win-check.exe'
$tmp = $env:RUNNER_TEMP; if (-not $tmp) { $tmp = $env:TEMP }
$failed = $false

function Check([string]$argstr, [bool]$mustPass) {
  $o = Join-Path $tmp 'o.txt'; $e = Join-Path $tmp 'e.txt'
  $p = Start-Process -FilePath $exe -ArgumentList $argstr -PassThru -NoNewWindow -RedirectStandardOutput $o -RedirectStandardError $e
  $null = $p.Handle
  $code = 'timeout'
  if ($p.WaitForExit(240000)) { $code = $p.ExitCode } else {
    Get-Process sgi-gpu-win-check -ErrorAction SilentlyContinue | Stop-Process -Force
  }
  "--- $argstr exit=$code"
  Get-Content $o -ErrorAction SilentlyContinue
  if ($code -ne 0) { Get-Content $e -ErrorAction SilentlyContinue | Select-Object -First 12 }
  if ($mustPass -and $code -ne 0) { $script:failed = $true }
}

Get-CimInstance Win32_OperatingSystem | Select-Object -ExpandProperty Caption
Get-CimInstance Win32_VideoController | Format-List Name, DriverVersion

"=== the fence orders the viewer's reads: every pixel of every frame must match"
Check '1280 1024 300 gpu' $true
Check '1920 1200 300 gpu' $true
Check '1280 1024 120 gpu 32' $true
"=== control: nothing orders the two queues, so slow composes should read stale frames"
Check '1280 1024 120 nowait 32' $false
if ($failed) { exit 1 }
