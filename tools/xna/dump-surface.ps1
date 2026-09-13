# The whole console native surface as machine-readable rows, for the generator
# in gen-exports.py:
#
#   module <TAB> entry <TAB> returnType <TAB> argTypes(csv) <TAB> verdict
#
# The verdict is what the CALLER does with the result - HANDLE, ERROR, ignored
# or unknown - resolved the same way classify-returns.ps1 does it, because that
# is the one fact a shim cannot be written without.
param([Parameter(Mandatory=$true)][string[]]$Assemblies)

$root = "F:\Xenia-Netplay2\build\bin\Windows\Release\xna"
Add-Type -Path (Join-Path $root "Mono.Cecil.dll")

foreach ($assembly in $Assemblies) {
  $path = Join-Path $root "console\$assembly"
  if (-not (Test-Path $path)) { continue }
  $resolver = New-Object Mono.Cecil.DefaultAssemblyResolver
  $resolver.AddSearchDirectory((Split-Path $path))
  $rp = New-Object Mono.Cecil.ReaderParameters
  $rp.AssemblyResolver = $resolver
  $module = [Mono.Cecil.ModuleDefinition]::ReadModule($path, $rp)

  $entryByMethod = @{}
  $sig = @{}
  foreach ($t in $module.GetTypes()) {
    foreach ($m in $t.Methods) {
      if (-not $m.PInvokeInfo) { continue }
      $key = $m.PInvokeInfo.Module.Name + "!" + $m.PInvokeInfo.EntryPoint
      $entryByMethod[$m.FullName] = $key
      if (-not $sig.ContainsKey($key)) {
        $args = @($m.Parameters | ForEach-Object {
          $p = $_.ParameterType
          $name = $p.FullName
          if ($p.IsByReference) { "ref:" + $name.TrimEnd('&') }
          elseif ($p.IsPointer) { "ptr:" + $name.TrimEnd('*') }
          elseif ($name -eq "System.Text.StringBuilder") { "sb" }
          elseif ($p.IsValueType -and -not $p.IsPrimitive) { "struct:" + $name }
          else { $name }
        })
        $sig[$key] = @{ ret = $m.ReturnType.FullName; args = ($args -join ",") }
      }
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
      $ins = @($m.Body.Instructions)
      for ($i = 0; $i -lt $ins.Count; $i++) {
        if (-not $ins[$i].Operand) { continue }
        if ($ins[$i].Operand.ToString() -notmatch "::([A-Za-z0-9_]+)\(") { continue }
        $short = $Matches[1]
        if (-not $byName.ContainsKey($short)) { continue }
        if ($ambiguous.ContainsKey($short)) { continue }
        $entry = $byName[$short]
        for ($j = $i + 1; $j -lt [Math]::Min($i + 8, $ins.Count); $j++) {
          $next = $ins[$j].ToString()
          if ($next -match "ThrowExceptionFromResult") { $verdict[$entry] = "ERROR"; break }
          if ($next -match "ldc.i4.m1") { if (-not $verdict.ContainsKey($entry)) { $verdict[$entry] = "HANDLE" }; break }
          if ($next -match "stfld.*pComPtr") { if (-not $verdict.ContainsKey($entry)) { $verdict[$entry] = "HANDLE" }; break }
        }
      }
    }
  }

  foreach ($key in ($sig.Keys | Sort-Object)) {
    $v = "unknown"
    if ($verdict.ContainsKey($key)) { $v = $verdict[$key] }
    Write-Output ("{0}`t{1}`t{2}`t{3}" -f $key, $sig[$key].ret, $sig[$key].args, $v)
  }
}
