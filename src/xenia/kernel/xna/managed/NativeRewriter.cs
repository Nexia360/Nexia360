// Captures the console's native calls by rewriting them, at load time.
//
// A P/Invoke is an extern method with no body: the runtime binds it to an
// export when it is first called, and there is no way to intercept that from
// managed code - DllImportResolver only chooses a MODULE, after which the
// runtime does its own lookup by name and fails on anything missing.
//
// So the extern is turned into an ordinary method instead. Its DllImport is
// stripped, and it is given a body that either forwards to an implementation
// in NativeCalls or reports itself and returns a default. The call sites do
// not change at all - they were already calling this method.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using Mono.Cecil;
using Mono.Cecil.Cil;

namespace Nexia.Xna;

internal static class NativeRewriter {
  // Modules that only exist on an Xbox. Anything a title P/Invokes outside
  // these is a real Windows library it brought itself, and must be left alone.
  private static readonly HashSet<string> ConsoleModules =
      new(StringComparer.OrdinalIgnoreCase) {
        "D3D", "XAM", "XINPUT", "STORAGE", "Net", "AUDIO", "Audio", "MEDIA",
        "SYSTEM",
      };

  /// <summary>
  /// Redirects every console P/Invoke in <paramref name="module"/> into
  /// <see cref="NativeCalls"/>. Returns how many were rewritten.
  /// </summary>
  public static int Rewrite(ModuleDefinition module) {
    var unimplemented = module.ImportReference(
        typeof(NativeCalls).GetMethod(nameof(NativeCalls.Unimplemented)));

    int count = 0;
    foreach (var type in AllTypes(module)) {
      foreach (var method in type.Methods) {
        if (!method.IsPInvokeImpl || method.PInvokeInfo == null) {
          continue;
        }
        var moduleName = method.PInvokeInfo.Module?.Name ?? "";
        // Kernel32 and friends: a title calling those is calling something
        // this machine genuinely has.
        if (!ConsoleModules.Contains(StripExtension(moduleName))) {
          continue;
        }

        var entry = method.PInvokeInfo.EntryPoint;
        if (string.IsNullOrEmpty(entry)) {
          entry = method.Name;
        }
        Implement(module, method, StripExtension(moduleName) + "!" + entry,
                  unimplemented);
        count++;
      }
    }
    return count;
  }

  // Mono.Cecil and System.Reflection both define MethodBody and
  // MethodImplAttributes, and both namespaces are in scope here - MethodInfo
  // and BindingFlags come from Reflection - so those two are spelled out.
  private static void Implement(ModuleDefinition module, MethodDefinition method,
                                string entry, MethodReference unimplemented) {
    // Stop being a P/Invoke, then give the method a body.
    //
    // The attribute is cleared directly rather than through IsPInvokeImpl:
    // going through the helper left MethodAttributes.PInvokeImpl set, so
    // Cecil still considered the method body-less and wrote no IL. The result
    // loads as a method with neither IL nor a P/Invoke, which the runtime can
    // only read as an internal call - "ECall methods must be packaged into a
    // system module" - from an assembly that is not a system module.
    method.PInvokeInfo = null;
    method.Attributes &= ~Mono.Cecil.MethodAttributes.PInvokeImpl;
    // IL and Managed are both zero, so this clears InternalCall, Native,
    // Unmanaged, Runtime and PreserveSig in one go.
    method.ImplAttributes = Mono.Cecil.MethodImplAttributes.IL |
                            Mono.Cecil.MethodImplAttributes.Managed;

    var body = new Mono.Cecil.Cil.MethodBody(method);
    method.Body = body;
    if (!method.HasBody) {
      // Cecil writes no IL for a method it thinks cannot have any, and the
      // failure would otherwise surface much later as an ECall error.
      XnaOs.Log(XnaOs.Level.Error,
                $"   {entry} still has no body after rewriting - it will fail "
                + "to JIT");
    }
    var il = body.GetILProcessor();

    var implementation = FindImplementation(method, entry);
    if (implementation != null) {
      var wanted = implementation.GetParameters();
      for (int i = 0; i < method.Parameters.Count; i++) {
        il.Append(il.Create(OpCodes.Ldarg, method.Parameters[i]));
        // A console entry point takes structures the host cannot name: they
        // are declared inside the console assembly, so no method here could
        // ever have a matching signature. Those arguments arrive as managed
        // pointers and are handed over as IntPtr instead, leaving the
        // implementation to read and write the memory itself.
        if (wanted[i].ParameterType == typeof(IntPtr) &&
            !(method.Parameters[i].ParameterType is PointerType)) {
          il.Append(il.Create(OpCodes.Conv_U));
        }
      }
      il.Append(il.Create(OpCodes.Call, module.ImportReference(implementation)));
      il.Append(il.Create(OpCodes.Ret));
      body.MaxStackSize = Math.Max(method.Parameters.Count, 1);
      return;
    }

    il.Append(il.Create(OpCodes.Ldstr, entry));
    il.Append(il.Create(OpCodes.Call, unimplemented));
    // An out parameter the console would have filled in is left holding
    // whatever the caller's stack had. FrameworkDispatcher reads its
    // notification word straight back and would act on rubbish, so every
    // by-reference argument is zeroed. initobj does the right thing for value
    // types and reference types alike.
    foreach (var parameter in method.Parameters) {
      if (!(parameter.ParameterType is ByReferenceType byRef)) {
        continue;
      }
      il.Append(il.Create(OpCodes.Ldarg, parameter));
      il.Append(il.Create(OpCodes.Initobj, byRef.ElementType));
    }
    EmitDefault(module, il, method.ReturnType, body);
    il.Append(il.Create(OpCodes.Ret));
    body.MaxStackSize = 8;
  }

  // An implementation is a public static method on NativeCalls named after the
  // entry point. Each parameter must either be exactly the console's type, or
  // IntPtr standing in for a by-reference or pointer argument - which is how a
  // structure declared inside the console assembly gets passed to code that
  // cannot name it. Anything else is treated as absent rather than called with
  // the wrong shape.
  private static MethodInfo FindImplementation(MethodDefinition method,
                                               string entry) {
    var name = entry.Replace("!", "_");
    var candidate = typeof(NativeCalls).GetMethod(
        name, BindingFlags.Public | BindingFlags.Static);
    if (candidate == null) {
      return null;
    }
    var parameters = candidate.GetParameters();
    if (parameters.Length != method.Parameters.Count) {
      return null;
    }
    for (int i = 0; i < parameters.Length; i++) {
      var wanted = parameters[i].ParameterType;
      var actual = method.Parameters[i].ParameterType;
      if (wanted.FullName == actual.FullName) {
        continue;
      }
      if (wanted == typeof(IntPtr) &&
          (actual is ByReferenceType || actual is PointerType)) {
        continue;
      }
      return null;
    }
    return candidate.ReturnType.FullName == method.ReturnType.FullName
        ? candidate
        : null;
  }

  // Whatever the method promised to return has to be produced, or the body is
  // not verifiable and will not JIT.
  private static void EmitDefault(ModuleDefinition module, ILProcessor il,
                                  TypeReference returnType,
                                  Mono.Cecil.Cil.MethodBody body) {
    if (returnType.FullName == "System.Void") {
      return;
    }
    if (returnType.IsValueType || returnType.IsGenericParameter) {
      // Covers every primitive and every enum without enumerating them: a
      // zeroed local of the right type.
      var local = new VariableDefinition(returnType);
      body.Variables.Add(local);
      body.InitLocals = true;
      il.Append(il.Create(OpCodes.Ldloca, local));
      il.Append(il.Create(OpCodes.Initobj, returnType));
      il.Append(il.Create(OpCodes.Ldloc, local));
      return;
    }
    il.Append(il.Create(OpCodes.Ldnull));
  }

  private static string StripExtension(string moduleName) {
    int dot = moduleName.LastIndexOf('.');
    return dot < 0 ? moduleName : moduleName.Substring(0, dot);
  }

  private static IEnumerable<TypeDefinition> AllTypes(ModuleDefinition module) {
    var pending = new Stack<TypeDefinition>(module.Types);
    while (pending.Count > 0) {
      var type = pending.Pop();
      yield return type;
      foreach (var nested in type.NestedTypes) {
        pending.Push(nested);
      }
    }
  }
}
