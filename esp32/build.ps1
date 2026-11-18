# Compila (e opcionalmente grava) o firmware ESP32 a partir do terminal.
# O ESP-IDF nao aceita espacos no caminho do projeto; se o repositorio estiver em uma
# pasta com espacos, este script copia esp32/ e src/huffman.* para C:\str_m3_esp32
# (mantendo a mesma estrutura) e compila la. Sem espacos, pode-se usar direto o
# PlatformIO na pasta esp32/ (ver platformio.ini).
# Uso:  powershell -File esp32\build.ps1 [-Upload] [-Port COM3] [-Demo]
#   -Demo : ambiente "demo"     (1.000 ativacoes por etapa, ~45 s)
#   sem   : ambiente "campanha" (10.000 / 3.000 / 10.000 / 10.000, ~6 min)
param([switch]$Upload, [string]$Port = "COM3", [switch]$Demo)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$dst  = "C:\str_m3_esp32"
New-Item -ItemType Directory -Force "$dst\esp32\src", "$dst\src" | Out-Null
Copy-Item "$here\platformio.ini","$here\CMakeLists.txt","$here\partitions.csv","$here\sdkconfig.defaults" "$dst\esp32" -Force
Copy-Item "$here\src\main.c","$here\src\CMakeLists.txt" "$dst\esp32\src" -Force
Copy-Item "$here\..\src\huffman.c","$here\..\src\huffman.h" "$dst\src" -Force
$envName = if ($Demo) { "demo" } else { "campanha" }
$pio = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
Push-Location "$dst\esp32"
$ErrorActionPreference = "Continue"
try {
  if ($Upload) { & $pio run -e $envName -t upload --upload-port $Port } else { & $pio run -e $envName }
  if ($LASTEXITCODE -ne 0) { throw "pio falhou ($LASTEXITCODE)" }
} finally { Pop-Location }
