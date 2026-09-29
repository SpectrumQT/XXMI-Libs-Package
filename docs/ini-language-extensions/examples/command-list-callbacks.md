# Command List Callbacks

Suppose a shared "library" INI handles a character's draw calls, and other mods want to run their own commands at a specific point of that handling, without editing the library. [Proxy command lists](../command-lists/README.md/#proxy-command-lists) let the library expose an empty callback that other namespaces redirect to their own command lists.

## Library

The library declares an empty `CommandListOnCharacterDraw` and runs it where the hook should fire:

```ini
; Mods\CharacterLib\lib.ini
namespace = CharacterLib

[TextureOverrideCharacter]
hash = 12345678
run = CommandListCharacter

[CommandListCharacter]
; Library work...
$character_drawn = 1

; Hook point. Empty by default, so this is a no-op until someone attaches to it.
run = CommandListOnCharacterDraw

[CommandListOnCharacterDraw]
```

## Consumer

Another mod attaches its handler once at startup by pointing the library's callback at a command list of its own:

```ini
; Mods\Outfit\outfit.ini
[Constants]
CommandList\CharacterLib\OnCharacterDraw = ref CommandListOutfitHandler

[CommandListOutfitHandler]
ps-t3 = ref ResourceOutfitTexture
```

`CommandList\CharacterLib\OnCharacterDraw` refers to the library's `[CommandListOnCharacterDraw]` section through its namespace. From now on, `run = CommandListOnCharacterDraw` in the library executes `CommandListOutfitHandler` instead.

Overriding a command list from a different namespace is only allowed while it is empty. This is what makes the hook safe: a consumer can attach to the empty callback, but cannot replace the library's real command lists.

## Detaching and Switching

The reference is a runtime state, so it can be changed by a key or a condition:

```ini
[KeyToggleOutfit]
key = F6
type = cycle
$outfit = 0, 1
run = CommandListApplyOutfit

[CommandListApplyOutfit]
if $outfit == 1
    CommandList\CharacterLib\OnCharacterDraw = ref CommandListOutfitHandler
else
    CommandList\CharacterLib\OnCharacterDraw = null ; Back to the empty callback.
endif
```

## Chaining Consumers

A consumer that wants to keep an existing handler in place can chain: point the callback at its own command list and run the previous handler from there.

```ini
; Mods\Effects\effects.ini
[Constants]
CommandList\CharacterLib\OnCharacterDraw = ref CommandListEffectsHandler

[CommandListEffectsHandler]
run = CommandList\Outfit\OutfitHandler ; Keep the outfit handler.
ps-t4 = ref ResourceEffectTexture
```

Assignments that would form a loop, such as the outfit handler referencing the effects handler back, are ignored, so a broken chain never recurses forever. See [Circular References](../command-lists/README.md/#circular-references).
