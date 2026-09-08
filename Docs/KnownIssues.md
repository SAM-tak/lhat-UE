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
