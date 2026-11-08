# Converts binary PPM images (P6) to PNG, e.g. the testbench output. Usage: ppm2png.ps1 <ppm files...>
param([Parameter(Mandatory, ValueFromRemainingArguments)][string[]]$Files)
Add-Type -AssemblyName System.Drawing
foreach ($f in $Files) {
    $bytes = [IO.File]::ReadAllBytes((Resolve-Path $f))
    # header: "P6", width, height, "255", separated by whitespace
    $pos = 0; $fields = @()
    while ($fields.Count -lt 4) {
        $start = $pos
        while ([char]$bytes[$pos] -notmatch '\s') { $pos++ }
        $fields += [Text.Encoding]::ASCII.GetString($bytes, $start, $pos - $start)
        $pos++
    }
    $w = [int]$fields[1]; $h = [int]$fields[2]
    $bmp = New-Object Drawing.Bitmap $w, $h
    for ($y = 0; $y -lt $h; $y++) {
        for ($x = 0; $x -lt $w; $x++) {
            $i = $pos + 3 * ($y * $w + $x)
            $bmp.SetPixel($x, $y, [Drawing.Color]::FromArgb($bytes[$i], $bytes[$i + 1], $bytes[$i + 2]))
        }
    }
    $out = [IO.Path]::ChangeExtension((Resolve-Path $f).Path, '.png')
    $bmp.Save($out, [Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    $out
}
