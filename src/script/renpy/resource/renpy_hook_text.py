# coding: utf-8
from __future__ import unicode_literals
import ctypes
import json
import re
import time
import ast
import difflib
import weakref
import renpy

try:
    _renpy_string_types = (basestring,)
    _renpy_unicode = unicode
except NameError:
    _renpy_string_types = (str,)
    _renpy_unicode = str

_renpy_open_tags = re.compile(r"^(?:\{(?!/)[A-Za-z][A-Za-z0-9_.:-]*(?:=[^{}\r\n]*)?\})+")
# Closing styles plus end-of-line advance/wait tags ({nw}, {w}, {p}); dropping
# them would change the original auto-advance and pause behaviour.
_renpy_close_tags = re.compile(r"(?:\{/[A-Za-z][A-Za-z0-9_.:-]*\}|\{(?:nw|w|p)(?:=[0-9.]+)?\})+$")

def _renpy_hook_functions():
    bits = ctypes.sizeof(ctypes.c_void_p) * 8
    hook = ctypes.CDLL(str("RenPyHook%d.dll" % bits))
    capture = hook.renpy_internal_capture_event
    capture.argtypes = (ctypes.c_wchar_p, ctypes.c_int)
    capture.restype = ctypes.c_wchar_p
    embed = hook.renpy_internal_embed_text
    embed.argtypes = (ctypes.c_wchar_p, ctypes.c_int)
    embed.restype = ctypes.c_wchar_p
    font = hook.renpy_internal_embed_font
    font.argtypes = ()
    font.restype = ctypes.c_wchar_p
    return capture, embed, font

_renpy_capture, _renpy_embed, _renpy_embed_font = _renpy_hook_functions()
_renpy_dialogue_event = ""
_renpy_dialogue_source = ""
_renpy_dialogue_raw = ""
_renpy_dialogue_translation = None
_renpy_dialogue_generation = 0
_renpy_dialogue_affixes = ("", "", "")
_renpy_adv_scope = 0
_renpy_menu_scope = None
_renpy_substitution_records = []
_renpy_slot_capture = None
_renpy_resource_report_at = 0
_renpy_screen_scope = None
_renpy_text_capture = None
_renpy_screen_texts = weakref.WeakKeyDictionary()
_renpy_screen_snapshots = weakref.WeakKeyDictionary()

def _renpy_unicode_text(value):
    if value is None:
        return ""
    if isinstance(value, (list, tuple)):
        return "".join(_renpy_unicode_text(item) for item in value)
    if isinstance(value, bytes) and not isinstance(value, _renpy_unicode):
        return value.decode("utf-8", "strict")
    return value if isinstance(value, _renpy_string_types) else _renpy_unicode(value)

def _renpy_plain_text(value):
    value = _renpy_unicode_text(value)
    extras = renpy.text.extras
    try:
        if hasattr(extras, "filter_text_tags"):
            value = extras.filter_text_tags(value, allow=set()).replace("{{", "{")
        else:
            # Ren'Py 6.99 has no filter_text_tags; its tokenizer already turns
            # "{{" into a literal brace inside TEXT tokens.
            support = renpy.text.textsupport
            value = "".join(text if kind == support.TEXT else "\n" if kind == support.PARAGRAPH else ""
                            for kind, text in support.tokenize(_renpy_unicode(value)))
    except Exception:
        pass
    return value.strip()

def _renpy_affixes(character, what):
    """Split Character what_prefix/what_suffix from the displayed string.

    Character.__call__ adds the affixes before do_display/display_say, but the
    AST, cache key and template contain only the core line. Only exact literal
    affixes are removed; interpolated affixes keep the visible text unchanged.
    """
    what = _renpy_unicode_text(what)
    prefix = _renpy_unicode_text(getattr(character, "what_prefix", "") or "")
    suffix = _renpy_unicode_text(getattr(character, "what_suffix", "") or "")
    if "[" in prefix or "[" in suffix or not (prefix or suffix):
        return "", what, ""
    if len(what) >= len(prefix) + len(suffix) and what.startswith(prefix) and what.endswith(suffix):
        return prefix, what[len(prefix):len(what) - len(suffix)], suffix
    return "", what, ""

def _renpy_emit(kind, text="", speaker="", skipping=False, split=1, **fields):
    payload = dict(kind=kind, speaker=_renpy_plain_text(speaker), text=_renpy_plain_text(text),
                   skipping=bool(skipping), page=_renpy_dialogue_generation)
    payload.update(fields)
    encoded = _renpy_unicode(json.dumps(payload, ensure_ascii=True, separators=(",", ":")))
    _renpy_capture(encoded, split)
    return encoded

def _renpy_is_fast_skipping():
    return getattr(renpy.config, "skipping", None) == "fast"

# The native embed path treats a text buffer above 1000 bytes (UTF-16) as
# already translated: it sends no request yet still waits the full timeout,
# freezing the game thread on every long line. Requests stay below that bound.
_renpy_embed_payload_limit = 900

def _renpy_embed_payload(text, speaker, template):
    """Compact embed request; source/original stay in the capture event only.

    Returns "" when even the compact request would exceed the native bound, so
    the line keeps its original text instead of blocking the game.
    """
    payload = dict(kind="dialogue", speaker=_renpy_plain_text(speaker), text=_renpy_plain_text(text),
                   skipping=False, page=_renpy_dialogue_generation, injectable=True)
    for binding in ((template,) if template else ()) + ("",):
        if binding:
            payload["template"] = binding
        else:
            payload.pop("template", None)
        encoded = _renpy_unicode(json.dumps(payload, ensure_ascii=True, separators=(",", ":")))
        if len(encoded) * 2 <= _renpy_embed_payload_limit:
            return encoded
    return ""

def _renpy_injected_text(original, translated):
    translated = translated.replace("{", "{{").replace("[", "[[")
    original = _renpy_unicode_text(original)
    opening = _renpy_open_tags.match(original)
    closing = _renpy_close_tags.search(original)
    # Only edge styles have a defined placement after translation. Internal control
    # tags (including paragraph boundaries) keep the complete original sentence.
    interior = original[opening.end() if opening else 0:closing.start() if closing else len(original)]
    if re.search(r"(?<!\{)\{(?!\{)", interior):
        return original
    return (opening.group(0) if opening else "") + translated + (closing.group(0) if closing else "")

def _renpy_begin_dialogue(original, plain_text):
    global _renpy_dialogue_source, _renpy_dialogue_raw, _renpy_dialogue_translation, _renpy_dialogue_generation
    _renpy_dialogue_source = plain_text
    _renpy_dialogue_raw = _renpy_unicode_text(original)
    _renpy_dialogue_translation = None
    _renpy_dialogue_generation += 1

def _renpy_current_translation():
    global _renpy_dialogue_translation
    if not _renpy_dialogue_source or not _renpy_dialogue_event or _renpy_is_fast_skipping():
        return ""
    if _renpy_dialogue_translation is None:
        translated = _renpy_embed(_renpy_dialogue_event, 1)
        _renpy_dialogue_translation = translated if translated and translated != _renpy_dialogue_event else ""
    return _renpy_dialogue_translation

def _renpy_wrap_display_say(original):
    def wrapped(who, what, show_function, *args, **kwargs):
        if not _renpy_adv_scope:
            return original(who, what, show_function, *args, **kwargs)
        generation = _renpy_dialogue_generation
        translated = _renpy_current_translation()
        replacement = what
        prefix, core, suffix = _renpy_dialogue_affixes
        if translated:
            # Affixes recorded for this page stay outside the translated line.
            if prefix + core + suffix == _renpy_unicode_text(what):
                replacement = prefix + _renpy_injected_text(core, translated) + suffix
            else:
                replacement = _renpy_injected_text(what, translated)
        if generation != _renpy_dialogue_generation or _renpy_is_fast_skipping():
            replacement = what
        def show(*show_args, **show_kwargs):
            result = show_function(*show_args, **show_kwargs)
            if replacement != what:
                widget = result
                if isinstance(widget, tuple):
                    widget = renpy.display.screen.get_widget(*widget)
                if isinstance(widget, renpy.text.text.Text):
                    font_path = _renpy_embed_font() or ""
                    if font_path:
                        widget.style.font = font_path.replace("\\", "/")
            return result
        # Pass the display copy before the engine computes slow-text indices.
        # Character history/store/rollback retain their original objects.
        return original(who, replacement, show, *args, **kwargs)
    wrapped._dango_renpy_original = original
    return wrapped

def _renpy_wrap_dialogue(original, adv=False):
    def wrapped(self, who, what, *args, **display_args):
        global _renpy_adv_scope, _renpy_dialogue_event, _renpy_dialogue_affixes
        _renpy_ensure_installed()
        _renpy_dialogue_affixes = _renpy_affixes(self, what)
        core = _renpy_dialogue_affixes[1]
        _renpy_begin_dialogue(core, _renpy_plain_text(core))
        _renpy_dialogue_event = ""
        if _renpy_is_fast_skipping():
            _renpy_emit("skip", skipping=True, split=3)
        else:
            identity = _renpy_source_identity(core)
            if adv:
                _renpy_dialogue_event = _renpy_embed_payload(core, who, identity.get("template", ""))
                if not _renpy_dialogue_event:
                    # Too long for an embed request: the client translates the
                    # captured page for the translation box, the game keeps text.
                    identity["embed_skip"] = True
            _renpy_emit("dialogue", text=core, speaker=who, injectable=adv, split=1, **identity)
        previous = _renpy_adv_scope
        _renpy_adv_scope = previous + 1 if adv and not _renpy_is_fast_skipping() else 0
        try:
            return original(self, who, what, *args, **display_args)
        finally:
            _renpy_adv_scope = previous
    wrapped._dango_renpy_original = original
    return wrapped

def _renpy_wrap_substitute(original):
    def wrapped(value, *args, **kwargs):
        global _renpy_slot_capture
        previous = _renpy_slot_capture
        capture = None
        try:
            fields = _renpy_fields(value)
            names = [name for _, name, fmt, conv in fields if name is not None]
            if names and all(re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", name or "") and not fmt and not conv
                             for _, name, fmt, conv in fields if name is not None):
                capture = [names, []]
        except Exception:
            pass
        _renpy_slot_capture = capture
        try:
            result = original(value, *args, **kwargs)
        finally:
            _renpy_slot_capture = previous
        if capture is not None and len(capture[0]) == len(capture[1]):
            _renpy_substitution_records.append((value, result[0], [dict(token="[" + name + "]", value=slot)
                                                 for name, slot in zip(capture[0], capture[1])]))
            del _renpy_substitution_records[:-16]
        if _renpy_text_capture is not None:
            _renpy_text_capture.append((value, result[0], capture))
        if _renpy_menu_scope is not None:
            for index, label in enumerate(_renpy_menu_scope[0]):
                if value == label:
                    _renpy_menu_scope[1][index] = _renpy_plain_text(result[0])
            if all(label is not None for label in _renpy_menu_scope[1]) and not _renpy_menu_scope[2]:
                _renpy_menu_scope[2] = True
                _renpy_emit("menu", text="\n".join(_renpy_menu_scope[1]), split=2)
        return result
    wrapped._dango_renpy_original = original
    return wrapped


def _renpy_fields(value):
    if hasattr(renpy.substitutions, "interpolate") and hasattr(renpy.substitutions, "parse"):
        return [(lit, name, fmt, conv) for lit, name, conv, fmt in renpy.substitutions.parse(value)]
    return list(renpy.substitutions.formatter.parse(value))


def _renpy_wrap_interpolate(original):
    def wrapped(value, scope):
        # 8.5 moved interpolation out of Formatter. Observe only the engine's
        # actual formatted values, without looking up/evaluating a name twice.
        if _renpy_slot_capture is not None:
            fields = _renpy_fields(value)
            names = [name for _, name, fmt, conv in fields if name is not None]
            if all(re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", name or "") and not fmt and not conv
                   for _, name, fmt, conv in fields if name is not None):
                _renpy_slot_capture[:] = [names, []]
            else:
                _renpy_slot_capture[:] = [[], [None]]
        return original(value, scope)
    wrapped._dango_renpy_original = original
    return wrapped


def _renpy_wrap_translate_string(original):
    def wrapped(value, *args, **kwargs):
        result = original(value, *args, **kwargs)
        if _renpy_screen_scope is not None and isinstance(value, _renpy_string_types) and isinstance(result, _renpy_string_types):
            _renpy_screen_scope.setdefault(result, set()).add(value)
        return result
    wrapped._dango_renpy_original = original
    return wrapped


def _renpy_wrap_set_text(original):
    def wrapped(widget, text, *args, **kwargs):
        global _renpy_text_capture
        previous = _renpy_text_capture
        capture = []
        _renpy_text_capture = capture
        try:
            result = original(widget, text, *args, **kwargs)
        finally:
            _renpy_text_capture = previous
        if _renpy_screen_scope is not None:
            raw = text if isinstance(text, _renpy_string_types) else None
            if isinstance(text, list) and len(text) == 1 and isinstance(text[0], _renpy_string_types):
                raw = text[0]
            if raw is not None:
                sources = _renpy_screen_scope.get(raw, set())
                source = next(iter(sources)) if len(sources) == 1 else raw
                identity = dict(source=_renpy_plain_text(source), original=_renpy_plain_text(source), template="")
                for value, rendered, slots in reversed(capture):
                    if value != raw or slots is None or len(slots[0]) != len(slots[1]):
                        continue
                    bindings = [dict(token="[" + name + "]", value=slot) for name, slot in zip(slots[0], slots[1])]
                    identity["template"] = json.dumps(dict(source=identity["source"], slots=bindings), ensure_ascii=True)
                    expanded = source
                    for binding in bindings:
                        expanded = expanded.replace(binding["token"], binding["value"])
                    identity["original"] = _renpy_plain_text(expanded)
                    break
                _renpy_screen_texts[widget] = identity
        return result
    wrapped._dango_renpy_original = original
    return wrapped


def _renpy_screen_literals(screen):
    result, seen = set(), set()
    pending = [getattr(screen.screen, "ast", None)]
    while pending and len(seen) < 10000:
        node = pending.pop()
        if node is None or id(node) in seen:
            continue
        seen.add(id(node))
        if type(node).__name__ == "SLDisplayable":
            for expression in getattr(node, "positional", [])[:1]:
                try:
                    value = ast.literal_eval(expression)
                    if isinstance(value, _renpy_string_types):
                        result.add(_renpy_plain_text(value))
                except (ValueError, TypeError, SyntaxError):
                    pass
        pending.extend(getattr(node, "children", []))
        pending.extend(entry[1] for entry in getattr(node, "entries", []) if len(entry) == 2)
    return result


def _renpy_wrap_screen_update(original):
    def wrapped(screen, *args, **kwargs):
        global _renpy_screen_scope, _renpy_dialogue_generation
        # Class patches survive utter_restart; the say screen updates every line.
        _renpy_ensure_installed()
        context = renpy.game.context()
        location = getattr(getattr(screen, "screen", None), "location", ("", 0))
        path = _renpy_unicode_text(location[0]).replace("\\", "/") if location else ""
        eligible = (not context.init_phase and not getattr(context, "_main_menu", False)
                    and not getattr(renpy.store, "main_menu", False)
                    and not getattr(renpy.display.predict, "predicting", False)
                    and not _renpy_adv_scope and _renpy_menu_scope is None
                    and "/renpy/common/" not in path and not path.startswith("renpy/common/")
                    and getattr(screen, "phase", None) not in (0, 3, 4))
        previous = _renpy_screen_scope
        if eligible:
            _renpy_screen_scope = {}
        returned = False
        try:
            result = original(screen, *args, **kwargs)
            returned = True
            if not eligible:
                return result
            literals = _renpy_screen_literals(screen)
            rows, seen = [], set()
            pending = [(getattr(screen, "child", None), False)]
            while pending and len(seen) < 10000:
                child, button = pending.pop()
                if child is None or id(child) in seen:
                    continue
                seen.add(id(child))
                button = button or isinstance(child, renpy.display.behavior.Button)
                if isinstance(child, renpy.text.text.Text):
                    identity = _renpy_screen_texts.get(child)
                    visible = _renpy_plain_text(getattr(child, "text", ""))
                    if identity and visible and len(visible) <= 8192 and identity["source"] not in literals and any(c.isalpha() for c in visible):
                        rows.append((visible, identity, button))
                children = child.visit()
                pending.extend((item, button) for item in reversed(children or []))
            keys = [(row[1]["source"], row[1]["original"], row[0], row[2]) for row in rows]
            old = _renpy_screen_snapshots.get(screen)
            _renpy_screen_snapshots[screen] = keys
            if _renpy_is_fast_skipping():
                if old != keys:
                    _renpy_emit("skip", skipping=True, split=3)
                return result
            if old is None:
                # Late attachment/new view: do not replay an entire chat history.
                selected = [row for row in rows if row[2]] or rows[-1:]
            else:
                selected = []
                for tag, i, j, a, b in difflib.SequenceMatcher(None, old, keys, autojunk=False).get_opcodes():
                    if tag in ("insert", "replace"):
                        selected.extend(rows[a:b])
            # One choice group is one page, preserving every button boundary.
            selected_buttons = [row for row in selected if row[2]]
            selected = [row for row in selected if not row[2]]
            for visible, identity, button in selected:
                _renpy_dialogue_generation += 1
                _renpy_emit("dialogue", text=visible, injectable=False, split=1, **identity)
            if selected_buttons:
                _renpy_dialogue_generation += 1
                buttons = [row for row in rows if row[2]]
                originals = [row[1]["original"] for row in buttons]
                _renpy_emit("menu", text="\n".join(row[0] for row in buttons), injectable=False, split=1,
                            source="\n".join(row[1]["source"] for row in buttons), original="\n".join(originals),
                            segments=originals)
            return result
        except Exception:
            # An unfamiliar tree disables this snapshot only. Exceptions from
            # the original consumer still propagate exactly as the engine did.
            if returned:
                return result
            raise
        finally:
            _renpy_screen_scope = previous
    wrapped._dango_renpy_original = original
    return wrapped

def _renpy_wrap_format_field(original):
    def wrapped(value, format_spec):
        result = original(value, format_spec)
        if _renpy_slot_capture is not None:
            _renpy_slot_capture[1].append(_renpy_unicode_text(result))
        return result
    wrapped._dango_renpy_original = original
    return wrapped

def _renpy_template(what):
    try:
        context = renpy.game.context()
        node = renpy.game.script.lookup(context.current)
        source = getattr(node, "what", None)
        for template, rendered, slots in reversed(_renpy_substitution_records):
            # An unknown filter or translated source must not impersonate the AST key.
            if source == template and rendered == what:
                return json.dumps(dict(source=_renpy_plain_text(source), slots=slots), ensure_ascii=True)
    except Exception:
        pass
    return ""

def _renpy_source_identity(what):
    """Associate the visible Say with its original engine Translate block.

    Never infer identity from translated text. Only simple string fields may be
    reconstructed; arbitrary expressions and custom Character conversions are
    not evaluated for collection.
    """
    result = dict(template=_renpy_template(what))
    try:
        context = renpy.game.context()
        node = renpy.game.script.lookup(context.current)
        if not isinstance(node, renpy.ast.Say):
            return result
        source_node = node
        language = renpy.game.preferences.language
        registered = getattr(renpy.store, "_dango_full_text_registered", None)
        if registered and registered[0] == language:
            translator = renpy.game.script.translator
            identifier = context.translate_identifier
            default = translator.default_translates.get(identifier)
            translated = translator.language_translates.get((identifier, language))
            if translated is not None:
                # Our generated blocks retain the original statement structure.
                # Unknown additions, changed speakers or an unrelated current
                # statement cannot acquire a source key.
                def statements(block):
                    return [block] if type(block).__name__ == "TranslateSay" else block.block
                if default is None or len(statements(default)) != len(statements(translated)):
                    return result
                index = next(i for i, child in enumerate(statements(translated)) if child is node)
                source_node = statements(default)[index]
                if not isinstance(source_node, renpy.ast.Say) or source_node.who != node.who:
                    return result
        observed = {}
        for template, rendered, slots in reversed(_renpy_substitution_records):
            if template == node.what and rendered == what:
                for slot in slots:
                    observed[slot["token"][1:-1]] = slot["value"]
                break
        source = _renpy_unicode_text(source_node.what)
        slots = []
        resolving = set()
        def plain_value(name):
            if name in observed:
                return observed[name]
            if name in resolving or len(resolving) >= 4:
                raise ValueError("cyclic name")
            resolving.add(name)
            try:
                value = renpy.store.__dict__.get(name)
                # Character.name is data only for the standard, non-dynamic
                # Character type. Do not call __str__/__format__ or callbacks.
                if type(value) is renpy.character.ADVCharacter:
                    state = value.__dict__
                    if state.get("dynamic"):
                        raise ValueError("dynamic Character")
                    value = state.get("name")
                if not isinstance(value, _renpy_string_types):
                    raise ValueError("non-string name")
                return render(value, False)
            finally:
                resolving.remove(name)
        def render(text, capture):
            pieces = []
            for literal, name, fmt, conv in _renpy_fields(text):
                pieces.append(literal)
                if name is not None:
                    if not re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", name or "") or fmt or conv:
                        raise ValueError("complex field")
                    value = plain_value(name)
                    pieces.append(value)
                    if capture:
                        slots.append(dict(token="[" + name + "]", value=value))
            return "".join(pieces)
        if _renpy_plain_text(render(node.what, False)) != _renpy_plain_text(what):
            return result
        result["source"] = _renpy_plain_text(source)
        try:
            original = render(source, True)
        except Exception:
            return result
        result["original"] = _renpy_plain_text(original)
        if slots:
            result["template"] = json.dumps(dict(source=result["source"], slots=slots), ensure_ascii=True)
    except Exception:
        pass
    return result

def _renpy_wrap_menu(original):
    def wrapped(items, *args, **kwargs):
        global _renpy_menu_scope, _renpy_dialogue_generation
        _renpy_dialogue_generation += 1
        previous = _renpy_menu_scope
        labels = [item[0] for item in items if item and len(item) >= 2 and item[1] is not None]
        _renpy_menu_scope = [labels, [None] * len(labels), False]
        if _renpy_is_fast_skipping():
            _renpy_emit("skip", skipping=True, split=3)
        elif labels and all("[" not in label for label in labels):
            _renpy_menu_scope[2] = True
            _renpy_emit("menu", text="\n".join(labels), split=2)
        try:
            # Labels, branch values, menuset and ChoiceReturn remain engine owned.
            return original(items, *args, **kwargs)
        finally:
            _renpy_menu_scope = previous
    wrapped._dango_renpy_original = original
    return wrapped

def _renpy_report_resources():
    global _renpy_resource_report_at
    now = time.time()
    if now - _renpy_resource_report_at < 1.0:
        return
    _renpy_resource_report_at = now
    language = getattr(renpy.store._preferences, "language", None)
    registered = getattr(renpy.store, "_dango_full_text_registered", None)
    digest, translation_digest = "", ""
    if registered and registered[0] == language:
        digest = getattr(renpy.store, "_dango_full_text_loaded_digest", "")
        translation_digest = registered[1]
    _renpy_emit("resources", language=language or "", digest=digest, translation_digest=translation_digest)

# Idempotence is checked on engine objects, including the NVL override.
# renpy.utter_restart() runs reload_all(): store modules and common scripts are
# rebuilt and config callback lists are restored from the startup backup, so
# store.menu/NVLCharacter wrappers and callbacks vanish while class patches stay.
def _renpy_installed():
    store = renpy.store
    for name in ("menu", "nvl_menu"):
        current = getattr(store, name, None)
        if current is not None and not hasattr(current, "_dango_renpy_original"):
            return False
    nvl = getattr(store, "NVLCharacter", None)
    if nvl is not None and not hasattr(nvl.do_display, "_dango_renpy_original"):
        return False
    if not hasattr(renpy.substitutions.formatter.format_field, "_dango_renpy_original"):
        return False
    for current in (renpy.character.display_say, renpy.substitutions.substitute, renpy.character.ADVCharacter.do_display):
        if not hasattr(current, "_dango_renpy_original"):
            return False
    return any(getattr(callback, "__name__", "") == "_renpy_report_resources" for callback in renpy.config.interact_callbacks)

def _renpy_install():
    for cls, adv in ((renpy.character.ADVCharacter, True), (getattr(renpy.store, "NVLCharacter", None), False)):
        if cls is not None:
            current = cls.do_display
            if not hasattr(current, "_dango_renpy_original"):
                cls.do_display = _renpy_wrap_dialogue(current, adv)
    current = renpy.character.display_say
    if not hasattr(current, "_dango_renpy_original"):
        renpy.character.display_say = _renpy_wrap_display_say(current)
    for name in ("menu", "nvl_menu"):
        current = getattr(renpy.store, name, None)
        if current is not None and not hasattr(current, "_dango_renpy_original"):
            setattr(renpy.store, name, _renpy_wrap_menu(current))
    current = renpy.substitutions.formatter.format_field
    if not hasattr(current, "_dango_renpy_original"):
        renpy.substitutions.formatter.format_field = _renpy_wrap_format_field(current)
    if hasattr(renpy.substitutions, "interpolate"):
        current = renpy.substitutions.interpolate
        if not hasattr(current, "_dango_renpy_original"):
            renpy.substitutions.interpolate = _renpy_wrap_interpolate(current)
        current = getattr(renpy.substitutions, "format", format)
        if not hasattr(current, "_dango_renpy_original"):
            renpy.substitutions.format = _renpy_wrap_format_field(current)
    current = renpy.substitutions.substitute
    if not hasattr(current, "_dango_renpy_original"):
        renpy.substitutions.substitute = _renpy_wrap_substitute(current)
    for owner, name, wrapper in ((renpy.translation, "translate_string", _renpy_wrap_translate_string),
                                 (renpy.text.text.Text, "set_text", _renpy_wrap_set_text),
                                 (renpy.display.screen.ScreenDisplayable, "update", _renpy_wrap_screen_update)):
        current = getattr(owner, name)
        if not hasattr(current, "_dango_renpy_original"):
            setattr(owner, name, wrapper(current))
    if not any(getattr(callback, "__name__", "") == "_renpy_report_resources" for callback in renpy.config.interact_callbacks):
        renpy.config.interact_callbacks.append(_renpy_report_resources)
        renpy.config.periodic_callbacks.append(_renpy_report_resources)

def _renpy_ensure_installed():
    # Cheap per-display check; reinstall only after the engine rebuilt itself.
    try:
        if not renpy.game.context().init_phase and not _renpy_installed():
            _renpy_install()
    except Exception:
        pass

_renpy_install()
_renpy_report_resources()
