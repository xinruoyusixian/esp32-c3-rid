$portName = "COM5"
$log = "C:\Users\Alan\Downloads\esp32-c3-rid-main\.workbuddy\ridtest\pair_out.txt"
"=== start $(Get-Date)" | Out-File -FilePath $log -Encoding utf8

$p = New-Object System.IO.Ports.SerialPort $portName,115200,None,8,One
$p.ReadTimeout = 1000
$p.WriteTimeout = 2000
$p.DtrEnable = $false
$p.RtsEnable = $false
$p.Encoding = [System.Text.Encoding]::UTF8

function Send-Cmd($cmd) {
    $p.WriteLine($cmd)
    Start-Sleep -Milliseconds 1200
    try { $t = $p.ReadExisting() } catch { $t = "" }
    return $t
}

try {
    $p.Open()
    Start-Sleep -Milliseconds 1500
    $null = $p.ReadExisting()

    $r = Send-Cmd "clearbonds"
    "clearbonds: " + ($r -replace "`r","") | Out-File -FilePath $log -Append -Encoding utf8

    $found = $false
    for ($i = 0; $i -lt 40; $i++) {
        $out = Send-Cmd "pair"
        if ($out -match "PAIRING\s+code=(\d{6})") {
            "PAIRING code=" + $Matches[1] | Out-File -FilePath $log -Append -Encoding utf8
            Start-Sleep -Milliseconds 300
            $p.WriteLine("oled dump")
            Start-Sleep -Milliseconds 3500
            try { $art = $p.ReadExisting() } catch { $art = "" }
            $art | Out-File -FilePath $log -Append -Encoding utf8
            $found = $true
            break
        }
        Start-Sleep -Milliseconds 800
    }
    if (-not $found) { "NO PAIRING TRIGGERED" | Out-File -FilePath $log -Append -Encoding utf8 }
} catch {
    "ERR: " + $_.Exception.Message | Out-File -FilePath $log -Append -Encoding utf8
} finally {
    if ($p.IsOpen) { $p.Close() }
}
"=== done $(Get-Date)" | Out-File -FilePath $log -Append -Encoding utf8
