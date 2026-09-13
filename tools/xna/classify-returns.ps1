# What does each console entry point's return value MEAN?
#
# There are only two conventions and they are not interchangeable:
#   HANDLE  - the caller stores it and compares against 0xFFFFFFFF (ldc.i4.m1)
#             to raise OutOfMemoryException. Any other value is accepted.
#   ERROR   - the caller passes it to ThrowExceptionFromResult, where anything
#             but 0 throws.
# Returning the wrong kind passes silently one way and kills the title the
# other, so this is worth deciding mechanically instead of per-shim by eye.
#
# Finds the managed wrapper for each P/Invoke, then every call site of that
# wrapper, and reports what the caller does with the result.
param([Parameter(Mandatory=$true)][string]$Assembly)

$root = Join-Path $PSScriptRoot "..\..\build\bin\Windows\Release\xna"
Add-Type -Path (Join-Path $root "Mono.Cecil.dll")
$path = Join-Path $root "console\$Assembly"
$resolver = New-Object Mono.Cecil.DefaultAssemblyResolver
$resolver.AddSearchDirectory((Split-Path $path))
$rp = New-Object Mono.Cecil.ReaderParameters
$rp.AssemblyResolver = $resolver
$module = [Mono.Cecil.ModuleDefinition]::ReadModule($path, $rp)

# Resolve method -> entry point, transitively.
#
# Callers do not call the P/Invoke directly: MXF.Graphics wraps each one in a
# virtual method on UnsafeNativeMethods/Device so the whole native surface can
# be swapped, so `CreateBlendState` is the wrapper and `InteropCreateBlendState`
# is the import. Any method whose body makes exactly one native call is treated
# as a wrapper for it, and that resolution is repeated until it stops growing.
$entryByMethod = @{}
foreach ($t in $module.GetTypes()) {
  foreach ($m in $t.Methods) {
    if ($m.PInvokeInfo) { $entryByMethod[$m.FullName] = $m.PInvokeInfo.EntryPoint }
  }
}
for ($pass = 0; $pass -lt 4; $pass++) {
  $added = 0
  foreach ($t in $module.GetTypes()) {
    foreach ($m in $t.Methods) {
      if (-not $m.HasBody) { continue }
      if ($entryByMethod.ContainsKey($m.FullName)) { continue }
      $hits = @()
      foreach ($i in $m.Body.Instructions) {
        if ($i.Operand -and $entryByMethod.ContainsKey($i.Operand.ToString())) {
          $hits += $entryByMethod[$i.Operand.ToString()]
        }
      }
      $hits = @($hits | Sort-Object -Unique)
      if ($hits.Count -eq 1) { $entryByMethod[$m.FullName] = $hits[0]; $added++ }
    }
  }
  if ($added -eq 0) { break }
}

$byName = @{}
$ambiguous = @{}
foreach ($full in $entryByMethod.Keys) {
  if ($full -notmatch "::([A-Za-z0-9_]+)\(") { continue }
  $short = $Matches[1]
  if ($byName.ContainsKey($short) -and $byName[$short] -ne $entryByMethod[$full]) {
    $ambiguous[$short] = $true
  }
  $byName[$short] = $entryByMethod[$full]
}

$verdict = @{}
foreach ($t in $module.GetTypes()) {
  foreach ($m in $t.Methods) {
    if (-not $m.HasBody) { continue }
    $instructions = @($m.Body.Instructions)
    for ($i = 0; $i -lt $instructions.Count; $i++) {
      $op = $instructions[$i].Operand
      if (-not $op) { continue }
      $callee = $op.ToString()
      if ($callee -notmatch "::([A-Za-z0-9_]+)\(") { continue }
      $short = $Matches[1]
      if (-not $byName.ContainsKey($short)) { continue }
      if ($ambiguous.ContainsKey($short)) { continue }
      $entry = $byName[$short]
      # Look at the next few instructions for the tell.
      $tell = "unknown"
      for ($j = $i + 1; $j -lt [Math]::Min($i + 8, $instructions.Count); $j++) {
        $next = $instructions[$j].ToString()
        if ($next -match "ThrowExceptionFromResult") { $tell = "ERROR"; break }
        if ($next -match "ldc.i4.m1") { $tell = "HANDLE"; break }
        if ($next -match "stfld.*pComPtr") { $tell = "HANDLE"; break }
        if ($next -match "^\s*IL_\w+: pop") { $tell = "ignored"; break }
      }
      if (-not $verdict.ContainsKey($entry)) { $verdict[$entry] = @{} }
      $verdict[$entry][$tell] = $true
    }
  }
}

foreach ($entry in ($verdict.Keys | Sort-Object)) {
  Write-Output ("{0,-42} {1}" -f $entry, (($verdict[$entry].Keys | Sort-Object) -join ", "))
}
