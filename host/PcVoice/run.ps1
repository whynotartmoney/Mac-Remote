$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

if (Get-Command py -ErrorAction SilentlyContinue) {
    py -3 -m pip install -r requirements.txt
    py -3 pc_voice.py @args
} elseif (Get-Command python -ErrorAction SilentlyContinue) {
    python -m pip install -r requirements.txt
    python pc_voice.py @args
} else {
    Write-Error "Install Python 3.9 or newer from https://www.python.org/downloads/windows/"
    exit 1
}
