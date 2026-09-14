# Pinned-core integration notes

## Type-level hostvalue calls lose arguments

Observed with LhatCore `ad5de8635a7063c20abd9c4187c145c676b0798e`,
frontend enabled, Win64 Release. No core changes are included in this plugin.

Register a 24-byte hostvalue type `ue.Vector`, then a member:

```cpp
lhat_register_hostvalue_member(program, "ue", "Vector", "new",
    "f^number^, number^, number^ -> ue.Vector;", callback, context);
```

Execute `actor.SetActorLocation(ue.Vector.new(1, 2, 3))` from a script callback.
In UE automation, the constructor callback was invoked with `count == 3`, but
all three argument tags were `LHAT_VALUE_NIL` and all three payloads were zero.

Code inspection points to a receiver-layout mismatch: `compile.c`'s
`width_of()` and method-call lowering reserve four slots for the hostvalue
typed receiver, while `vm.c`'s host-call argument gathering sees the type's
runtime members table, skips one receiver slot, and reads the padding as the
three arguments. This is a likely cause, not an upstream fix verified here.

The plugin instead registers a module-level function:

```cpp
lhat_register_func(program, "ue", "MakeVector",
    "f^number^, number^, number^ -> ue.Vector;", callback, context);
```

`Lhat.Runtime.ReflectedCalls` covers this supported route end-to-end, including
the nested call into SetActorLocation. Revisit type-level constructors after
a core fix and a corresponding regression test; do not silently treat nil
arguments as zero-valued numbers.

## Redundant parent/child imports fault at runtime

Observed with LhatCore `20008749c496afd73b7af1223fdcdb230eb402d0`, frontend
enabled, Win64 Release. A standalone C host reproduces this without Unreal:
register functions in both `ue` and `ue.Engine`, install the program, then run:

```text
import^ue
import^ue.Engine
```

Checking succeeds, but execution faults on the second import with
"this table belongs to the machine; what it holds is written by the host,
not from here". The core's existing parent/child import tests exercise checking,
not execution. No upstream fix is made here.

Importing the parent **once** is sufficient to reach its registered children:

```text
import^ue
let^ value = ue.Engine.KismetMathLibrary.Abs(-42.0)
let^ vector = ue.MakeVector(1, 2, 3)
```

Importing only children also works. When separate module aliases are wanted,
use the expression form, for example `let^ Engine = import^ue.Engine`.
`Lhat.Editor.GeneratedNativeScript` covers the parent-only route with both
generated Runtime/Editor functions and a vector argument/return.

## Wide hostvalue returns at the C call boundary

In the same pinned revision, `lhat_machine_call_member` intentionally converts
a hostvalue return into `nil`, because that C entry boundary has no result slots
for wide values. This does not indicate a failed native callback. Inspect such
results inside Lhat code (for example, return the vector's numeric fields),
where the VM reserves the correct slots. The generated native integration test
uses this route; the language core is unchanged.
