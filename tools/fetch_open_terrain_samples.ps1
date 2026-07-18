[CmdletBinding()]
param(
    [string]$OutputDirectory = (Join-Path $PSScriptRoot "..\dist\sample_data\open_data"),
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
[System.IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null

$qgisCsv = Join-Path $OutputDirectory "qgis_alaska_elevp.csv"
$qgisXyz = Join-Path $OutputDirectory "qgis_alaska_elevp.xyz"
$fujiDem = Join-Path $OutputDirectory "copernicus_glo90_fuji_n35_e138.tif"

$downloads = @(
    [pscustomobject]@{
        Name = "QGIS Alaska elevation points"
        Uri = "https://raw.githubusercontent.com/qgis/QGIS-Sample-Data/master/qgis_sample_data/csv/elevp.csv"
        Destination = $qgisCsv
    },
    [pscustomobject]@{
        Name = "Copernicus DEM GLO-90 N35E138"
        Uri = "https://copernicus-dem-90m.s3.eu-central-1.amazonaws.com/Copernicus_DSM_COG_30_N35_00_E138_00_DEM/Copernicus_DSM_COG_30_N35_00_E138_00_DEM.tif"
        Destination = $fujiDem
    }
)

foreach ($download in $downloads) {
    if ((Test-Path -LiteralPath $download.Destination) -and -not $Force) {
        Write-Host "Using existing file: $($download.Destination)"
        continue
    }

    $partial = "$($download.Destination).part"
    if (Test-Path -LiteralPath $partial) {
        Remove-Item -LiteralPath $partial -Force
    }

    Write-Host "Downloading $($download.Name)..."
    Invoke-WebRequest -Uri $download.Uri -OutFile $partial -UseBasicParsing
    Move-Item -LiteralPath $partial -Destination $download.Destination -Force
}

# The upstream CSV contains a header. dterrain point text deliberately accepts
# only numeric XYZ records and comments, so create a headerless, space-delimited
# file that can be selected directly with the GUI's "导入XYZ" command.
$rows = @(Import-Csv -LiteralPath $qgisCsv -Delimiter ";")
if ($rows.Count -ne 150) {
    throw "Unexpected QGIS point count: $($rows.Count); expected 150."
}

$xyzLines = foreach ($row in $rows) {
    $x = 0.0
    $y = 0.0
    $z = 0.0
    if (-not [double]::TryParse($row.X, [Globalization.NumberStyles]::Float,
            [Globalization.CultureInfo]::InvariantCulture, [ref]$x) -or
        -not [double]::TryParse($row.Y, [Globalization.NumberStyles]::Float,
            [Globalization.CultureInfo]::InvariantCulture, [ref]$y) -or
        -not [double]::TryParse($row.ELEV, [Globalization.NumberStyles]::Float,
            [Globalization.CultureInfo]::InvariantCulture, [ref]$z)) {
        throw "The QGIS CSV contains a non-numeric XYZ record."
    }
    "$($row.X) $($row.Y) $($row.ELEV)"
}
[System.IO.File]::WriteAllLines($qgisXyz, $xyzLines,
    [System.Text.Encoding]::ASCII)

$expectedHashes = @{
    "qgis_alaska_elevp.csv" = "E9F0605BEDDBAC25ACDE580A5F62FD575BB1507D98C471568E12C4F812019D26"
    "qgis_alaska_elevp.xyz" = "6A7264B1F627C8F63AAFBFF5F4A1BB870DE01A5E1674A6F58EC754511A7EEC48"
    "copernicus_glo90_fuji_n35_e138.tif" = "28397903E323701DBD541EBDE95C19A227CB9EAF1D520CDE2379ABF6D83B2AAC"
}

$results = foreach ($path in @($qgisCsv, $qgisXyz, $fujiDem)) {
    $item = Get-Item -LiteralPath $path
    $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    $expected = $expectedHashes[$item.Name]
    [pscustomobject]@{
        File = $item.Name
        Bytes = $item.Length
        SHA256 = $hash
        Verified = ($hash -eq $expected)
    }
}

$results | Format-Table -AutoSize
if ($results.Verified -contains $false) {
    Write-Warning "An upstream file differs from the version validated by this project. Review its source before use."
}

Write-Host "Samples are ready in: $OutputDirectory"
Write-Host "Trial guide: docs\OPEN_DATA_TRIAL_GUIDE.md"
