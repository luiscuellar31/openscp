# Reads what the Windows packaging and its checks need from a PE image: the
# machine, the DLL characteristics, CET compatibility and the imported DLLs.

$PeMachineAmd64 = 0x8664
$PeDynamicBase = 0x0040
$PeHighEntropyVa = 0x0020
$PeNxCompat = 0x0100
$PeGuardCf = 0x4000

function Read-PeImage([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    function U16([long]$Offset) { [BitConverter]::ToUInt16($bytes, $Offset) }
    function U32([long]$Offset) { [BitConverter]::ToUInt32($bytes, $Offset) }

    if ($bytes.Length -lt 0x40 -or (U16 0) -ne 0x5A4D) {
        throw "$Path is not a PE image."
    }
    $header = U32 0x3C
    if ($header + 24 -gt $bytes.Length -or (U32 $header) -ne 0x00004550) {
        throw "$Path has no PE header."
    }
    $coff = $header + 4
    $machine = U16 $coff
    $sectionCount = U16 ($coff + 2)
    $optionalSize = U16 ($coff + 16)
    $optional = $coff + 20
    if ((U16 $optional) -ne 0x20B) {
        throw "$Path is not a 64-bit image."
    }
    $dllCharacteristics = U16 ($optional + 70)
    $directoryCount = U32 ($optional + 108)
    $directories = $optional + 112

    $sections = for ($index = 0; $index -lt $sectionCount; $index++) {
        $section = $optional + $optionalSize + 40 * $index
        [pscustomobject]@{
            Address    = U32 ($section + 12)
            Size       = [Math]::Max((U32 ($section + 8)), (U32 ($section + 16)))
            RawPointer = U32 ($section + 20)
        }
    }
    # The file offset of a relative virtual address.
    function Get-Offset([long]$Rva) {
        foreach ($section in $sections) {
            if ($Rva -ge $section.Address -and $Rva -lt $section.Address + $section.Size) {
                return $Rva - $section.Address + $section.RawPointer
            }
        }
        throw "$Path has an address outside its sections."
    }
    function Read-CString([long]$Offset) {
        $end = $Offset
        while ($bytes[$end] -ne 0) { $end++ }
        return [Text.Encoding]::ASCII.GetString($bytes, $Offset, $end - $Offset)
    }
    function Get-Directory([int]$Index) {
        if ($Index -ge $directoryCount) { return @(0, 0) }
        return @((U32 ($directories + 8 * $Index)), (U32 ($directories + 8 * $Index + 4)))
    }

    $imports = [Collections.Generic.List[string]]::new()
    # Import descriptors are 20 bytes, with the DLL name at 12, up to a zero
    # entry.
    $importTable = (Get-Directory 1)[0]
    if ($importTable) {
        $entry = Get-Offset $importTable
        while ((U32 ($entry + 12)) -ne 0) {
            $imports.Add((Read-CString (Get-Offset (U32 ($entry + 12)))))
            $entry += 20
        }
    }
    # Delay-load descriptors are 32 bytes, with the DLL name at 4.
    $delayTable = (Get-Directory 13)[0]
    if ($delayTable) {
        $entry = Get-Offset $delayTable
        while ((U32 ($entry + 4)) -ne 0) {
            $imports.Add((Read-CString (Get-Offset (U32 ($entry + 4)))))
            $entry += 32
        }
    }

    # /CETCOMPAT is a debug directory entry of type 20 whose data has bit 0.
    $cetCompatible = $false
    $debugTable = Get-Directory 6
    if ($debugTable[0]) {
        $first = Get-Offset $debugTable[0]
        for ($entry = $first; $entry -lt $first + $debugTable[1]; $entry += 28) {
            $data = U32 ($entry + 24)
            if ((U32 ($entry + 12)) -eq 20 -and $data -and ((U32 $data) -band 1)) {
                $cetCompatible = $true
            }
        }
    }

    return [pscustomobject]@{
        Path               = $Path
        Machine            = $machine
        DllCharacteristics = $dllCharacteristics
        CetCompatible      = $cetCompatible
        Imports            = $imports.ToArray()
    }
}
