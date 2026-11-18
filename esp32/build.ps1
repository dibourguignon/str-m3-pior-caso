# Compila (e opcionalmente grava) o firmware ESP32.
# O ESP-IDF nao aceita espacos no caminho do projeto, entao o projeto e copiado para
# C:\str_m3_esp32 junto com src/huffman.c e src/huffman.h (mesmo codigo do Linux).
# Uso:  powershell -File esp32\build.ps1 [-Upload] [-Port COM3] [-Demo]
#   -Demo : 1.000 ativacoes por etapa (~1 min), para demonstracao ao vivo.
#           Sem -Demo: campanha completa do relatorio (10.000 / 3.000 / 10.000 / 10.000, ~6 min).
param([switch]$Upload, [string]$Port = "COM3", [switch]$Demo)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$dst  = "C:\str_m3_esp32"
New-Item -ItemType Directory -Force "$dst\src" | Out-Null
Copy-Item "$here\platformio.ini","$here\CMakeLists.txt","$here\partitions.csv","$here\sdkconfig.defaults" $dst -Force
Copy-Item "$here\src\main.c","$here\src\CMakeLists.txt" "$dst\src" -Force
Copy-Item "$here\..\src\huffman.c","$here\..\src\huffman.h" "$dst\src" -Force
# modo de compilacao: build_mode.h e lido por main.c (sem o arquivo, compila a campanha completa)
$mode = if ($Demo) { "#define DEMO 1`n" } else { "/* campanha completa */`n" }
[IO.File]::WriteAllText("$dst\src\build_mode.h", $mode)
$pio = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
Push-Location $dst
$ErrorActionPreference = "Continue"
try {
  if ($Upload) { & $pio run -t upload --upload-port $Port } else { & $pio run }
  if ($LASTEXITCODE -ne 0) { throw "pio falhou ($LASTEXITCODE)" }
} finally { Pop-Location }
