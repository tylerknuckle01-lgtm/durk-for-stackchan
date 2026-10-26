$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $dir
$running = Get-CimInstance Win32_Process -Filter "Name='python.exe'" | Where-Object { $_.CommandLine -like '*app.py*' -and $_.ExecutablePath -like "$dir*" }
if ($running) { Write-Output "Durk server already running (PID $($running.ProcessId -join ','))"; exit 0 }
$env:PATH = "$dir\venv\Scripts;$dir;" + [Environment]::GetEnvironmentVariable('Path','User') + ';' + [Environment]::GetEnvironmentVariable('Path','Machine')
$env:PYTHONIOENCODING = 'utf-8'
Start-Process -FilePath "$dir\venv\Scripts\python.exe" -ArgumentList app.py -WorkingDirectory $dir -RedirectStandardOutput "$dir\server.log" -RedirectStandardError "$dir\server.err" -WindowStyle Hidden
Write-Output "Durk server started"
