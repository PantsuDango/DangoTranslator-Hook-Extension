import ctypes
import json
import re


_renpy_open_tags = re.compile(r"^(?:\{(?!/)[A-Za-z][A-Za-z0-9_.:-]*(?:=[^{}\r\n]*)?\})+")
_renpy_close_tags = re.compile(r"(?:\{/[A-Za-z][A-Za-z0-9_.:-]*\})+$")


def _renpy_hook_functions():
    hook = ctypes.CDLL("RenPyHook64.dll")
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

_renpy_dialogue_source = ""
_renpy_dialogue_raw = ""
_renpy_dialogue_translation = None
_renpy_dialogue_generation = 0


def _renpy_plain_text(value):
    if value is None:
        return ""
    if isinstance(value, (list, tuple)):
        value = "".join(_renpy_plain_text(item) for item in value)
    elif not isinstance(value, str):
        value = str(value)
    try:
        value = renpy.text.extras.filter_text_tags(value, allow=set()).replace("{{", "{")
    except Exception:
        pass
    return value.strip()


def _renpy_emit(kind, text="", speaker="", skipping=False, split=1):
    payload = json.dumps(
        {
            "kind": kind,
            "speaker": _renpy_plain_text(speaker),
            "text": _renpy_plain_text(text),
            "skipping": bool(skipping),
        },
        ensure_ascii=False,
        separators=(",", ":"),
    )
    _renpy_capture(payload, split)


def _renpy_is_fast_skipping():
    return getattr(renpy.config, "skipping", None) == "fast"


def _renpy_injected_text(original, translated):
    translated = translated.replace("{", "{{")
    if not isinstance(original, str):
        return translated
    opening = _renpy_open_tags.match(original)
    closing = _renpy_close_tags.search(original)
    opening_text = opening.group(0) if opening else ""
    closing_text = closing.group(0) if closing else ""
    return opening_text + translated + closing_text


def _renpy_begin_dialogue(original, plain_text):
    global _renpy_dialogue_source
    global _renpy_dialogue_raw
    global _renpy_dialogue_translation
    global _renpy_dialogue_generation

    _renpy_dialogue_source = plain_text
    _renpy_dialogue_raw = original if isinstance(original, str) else plain_text
    _renpy_dialogue_translation = None
    _renpy_dialogue_generation += 1


def _renpy_current_translation():
    global _renpy_dialogue_translation

    if not _renpy_dialogue_source:
        return ""
    if _renpy_dialogue_translation is None:
        translated = _renpy_embed(_renpy_dialogue_source, 1)
        if translated and translated != _renpy_dialogue_source:
            _renpy_dialogue_translation = translated
        else:
            _renpy_dialogue_translation = ""
    return _renpy_dialogue_translation


def _renpy_replace_dialogue_text(value):
    if not _renpy_dialogue_source or _renpy_plain_text(value) != _renpy_dialogue_source:
        return value, False

    translated = _renpy_current_translation()
    if not translated:
        return value, True

    if isinstance(value, list):
        return [_renpy_injected_text(_renpy_dialogue_raw, translated)], True
    return _renpy_injected_text(value, translated), True


def _renpy_wrap_text_init(original):
    def wrapped(self, *args, **kwargs):
        matched = False
        injected = False
        if args:
            source = args[0]
            replacement, matched = _renpy_replace_dialogue_text(source)
            if matched:
                args = (replacement,) + args[1:]
                injected = replacement != source
        elif "text" in kwargs:
            source = kwargs["text"]
            replacement, matched = _renpy_replace_dialogue_text(source)
            if matched:
                kwargs["text"] = replacement
                injected = replacement != source

        if injected:
            font_path = _renpy_embed_font() or ""
            if font_path:
                kwargs["font"] = font_path.replace("\\", "/")

        result = original(self, *args, **kwargs)
        if matched:
            self._dango_renpy_generation = _renpy_dialogue_generation
        return result

    wrapped._dango_renpy_original = original
    return wrapped


def _renpy_wrap_text_render(original):
    def wrapped(self, *args, **kwargs):
        if getattr(self, "_dango_renpy_generation", 0) != _renpy_dialogue_generation:
            replacement, matched = _renpy_replace_dialogue_text(getattr(self, "text", None))
            if matched:
                if replacement != self.text:
                    self.set_text(replacement)
                self._dango_renpy_generation = _renpy_dialogue_generation
        return original(self, *args, **kwargs)

    wrapped._dango_renpy_original = original
    return wrapped


def _renpy_wrap_dialogue(original):
    def wrapped(self, who, what, **display_args):
        if _renpy_is_fast_skipping():
            _renpy_emit("skip", skipping=True, split=3)
            return original(self, who, what, **display_args)

        plain_text = _renpy_plain_text(what)
        _renpy_emit("dialogue", text=plain_text, speaker=who, split=1)
        if plain_text:
            _renpy_begin_dialogue(what, plain_text)
        return original(self, who, what, **display_args)

    wrapped._dango_renpy_original = original
    return wrapped


def _renpy_wrap_menu(original):
    def wrapped(items, *args, **kwargs):
        if _renpy_is_fast_skipping():
            _renpy_emit("skip", skipping=True, split=3)
        else:
            choices = []
            for item in items:
                if not item or len(item) < 2 or item[1] is None:
                    continue
                label = _renpy_plain_text(item[0])
                if label:
                    choices.append(label)
            if choices:
                _renpy_emit("menu", text="\n".join(choices), split=2)
        return original(items, *args, **kwargs)

    wrapped._dango_renpy_original = original
    return wrapped


try:
    import renpy

    current_display = renpy.character.ADVCharacter.do_display
    if not hasattr(current_display, "_dango_renpy_original"):
        renpy.character.ADVCharacter.do_display = _renpy_wrap_dialogue(current_display)

    current_text_init = renpy.text.text.Text.__init__
    if not hasattr(current_text_init, "_dango_renpy_original"):
        renpy.text.text.Text.__init__ = _renpy_wrap_text_init(current_text_init)

    current_text_render = renpy.text.text.Text.render
    if not hasattr(current_text_render, "_dango_renpy_original"):
        renpy.text.text.Text.render = _renpy_wrap_text_render(current_text_render)

    for menu_name in ("menu", "nvl_menu"):
        current_menu = getattr(renpy.store, menu_name, None)
        if current_menu is not None and not hasattr(current_menu, "_dango_renpy_original"):
            setattr(renpy.store, menu_name, _renpy_wrap_menu(current_menu))
except Exception:
    raise
