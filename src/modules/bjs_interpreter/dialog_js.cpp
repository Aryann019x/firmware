#if !defined(LITE_VERSION) && !defined(DISABLE_INTERPRETER)
#include "dialog_js.h"
#include "core/scrollableTextArea.h"

#include "helpers_js.h"
#include "keyboard_js.h"
#include "user_classes_js.h"

typedef struct {
    ScrollableTextArea *area;
} TextViewerData;

JSValue native_dialogMessage(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // Copy the labels into String objects.  JS_ToCString() may return a
    // pointer into a short-lived JSCStringBuf, and the old code kept those
    // pointers after the buffer went out of scope.
    String leftButton;
    String centerButton;
    String rightButton;
    if (argc > 1 && JS_IsObject(ctx, argv[1])) {
        JSValue v = JS_GetPropertyStr(ctx, argv[1], "left");
        if (JS_IsString(ctx, v)) {
            JSCStringBuf sb;
            const char *s = JS_ToCString(ctx, v, &sb);
            if (s) leftButton = s;
        }
        v = JS_GetPropertyStr(ctx, argv[1], "center");
        if (JS_IsString(ctx, v)) {
            JSCStringBuf sb;
            const char *s = JS_ToCString(ctx, v, &sb);
            if (s) centerButton = s;
        }
        v = JS_GetPropertyStr(ctx, argv[1], "right");
        if (JS_IsString(ctx, v)) {
            JSCStringBuf sb;
            const char *s = JS_ToCString(ctx, v, &sb);
            if (s) rightButton = s;
        }
    }
    if (argc < 1 || !JS_IsString(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "dialog.message(msg:string, buttons?:object)");
    JSCStringBuf msb;
    const char *msg = JS_ToCString(ctx, argv[0], &msb);
    if (!msg) return JS_ThrowTypeError(ctx, "dialog.message: invalid message");

    const char *left = leftButton.length() ? leftButton.c_str() : nullptr;
    const char *center = centerButton.length() ? centerButton.c_str() : nullptr;
    const char *right = rightButton.length() ? rightButton.c_str() : nullptr;
    int8_t selectedButton = displayMessage(msg, left, center, right, bruceConfig.priColor);

    if (selectedButton == 0) {
        if (left) return JS_NewString(ctx, left);
        if (center) return JS_NewString(ctx, center);
        if (right) return JS_NewString(ctx, right);
    } else if (selectedButton == 1) {
        if (center) return JS_NewString(ctx, center);
        if (right) return JS_NewString(ctx, right);
    } else if (right) {
        return JS_NewString(ctx, right);
    }
    return JS_NewString(ctx, "");
}

JSValue native_dialogInfo(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    if (argc < 1 || !JS_IsString(ctx, argv[0])) return JS_UNDEFINED;
    JSCStringBuf sb;
    const char *s = JS_ToCString(ctx, argv[0], &sb);
    bool wait = false;
    if (argc > 1 && JS_IsBool(argv[1])) wait = JS_ToBool(ctx, argv[1]);
    displayInfo(s, wait);
    return JS_UNDEFINED;
}

JSValue native_dialogSuccess(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    if (argc < 1 || !JS_IsString(ctx, argv[0])) return JS_UNDEFINED;
    JSCStringBuf sb;
    const char *s = JS_ToCString(ctx, argv[0], &sb);
    bool wait = false;
    if (argc > 1 && JS_IsBool(argv[1])) wait = JS_ToBool(ctx, argv[1]);
    displaySuccess(s, wait);
    return JS_UNDEFINED;
}

JSValue native_dialogWarning(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    if (argc < 1 || !JS_IsString(ctx, argv[0])) return JS_UNDEFINED;
    JSCStringBuf sb;
    const char *s = JS_ToCString(ctx, argv[0], &sb);
    bool wait = false;
    if (argc > 1 && JS_IsBool(argv[1])) wait = JS_ToBool(ctx, argv[1]);
    displayWarning(s, wait);
    return JS_UNDEFINED;
}

JSValue native_dialogError(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    if (argc < 1 || !JS_IsString(ctx, argv[0])) return JS_UNDEFINED;
    JSCStringBuf sb;
    const char *s = JS_ToCString(ctx, argv[0], &sb);
    bool wait = false;
    if (argc > 1 && JS_IsBool(argv[1])) wait = JS_ToBool(ctx, argv[1]);
    displayError(s, wait);
    return JS_UNDEFINED;
}

JSValue native_dialogPickFile(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    String r = "";
    String filepath = "/";
    String extension = "*";
    if (argc > 0 && JS_IsString(ctx, argv[0])) {
        JSCStringBuf sb;
        const char *s = JS_ToCString(ctx, argv[0], &sb);
        if (s) {
            filepath = String(s);
            if (!filepath.startsWith("/")) filepath = "/" + filepath;
        }
    }
    if (argc > 1 && JS_IsString(ctx, argv[1])) {
        JSCStringBuf sb;
        const char *s = JS_ToCString(ctx, argv[1], &sb);
        if (s) extension = String(s);
    }
    FS *fs = NULL;
    if (SD.exists(filepath)) fs = &SD;
    else if (LittleFS.exists(filepath)) fs = &LittleFS; // ← added 'else'
    if (fs) { r = loopSD(*fs, true, extension, filepath); }
    return JS_NewString(ctx, r.c_str());
}

JSValue native_dialogChoice(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: dialogChoice(choices : string[] | [key: string, value: string][] | {[key: string]: string})
    // returns: string or empty string
    String result;
    if (argc < 1 || !JS_IsObject(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "dialogChoice: Choice argument must be object or array.");
    }
    options.clear();

    if (JS_GetClassID(ctx, argv[0]) == JS_CLASS_ARRAY) {
        uint32_t arrayLength = 0;
        JSValue l = JS_GetPropertyStr(ctx, argv[0], "length");
        if (JS_IsNumber(ctx, l)) { JS_ToUint32(ctx, &arrayLength, l); }

        for (uint32_t i = 0; i < arrayLength; ++i) {
            JSValue jsvChoice = JS_GetPropertyUint32(ctx, argv[0], i);

            if (JS_IsString(ctx, jsvChoice)) {
                JSCStringBuf sb;
                const char *s = JS_ToCString(ctx, jsvChoice, &sb);
                if (!s) continue;
                String choice = s;
                options.push_back({choice, [&result, choice]() { result = choice; }});
            } else if (JS_GetClassID(ctx, jsvChoice) == JS_CLASS_ARRAY) {
                /* element is expected to be [key, value] */
                JSValue jsvInnerArrayLength = JS_GetPropertyStr(ctx, jsvChoice, "length");
                uint32_t innerArrayLength = 0;
                if (JS_IsNumber(ctx, jsvInnerArrayLength)) {
                    JS_ToUint32(ctx, &innerArrayLength, jsvInnerArrayLength);
                }
                if (innerArrayLength >= 2) {
                    JSValue jsvKey = JS_GetPropertyUint32(ctx, jsvChoice, 0);
                    JSValue jsvValue = JS_GetPropertyUint32(ctx, jsvChoice, 1);
                    if (JS_IsString(ctx, jsvKey) && JS_IsString(ctx, jsvValue)) {
                        JSCStringBuf keyBuf;
                        JSCStringBuf valueBuf;
                        const char *keyPtr = JS_ToCString(ctx, jsvKey, &keyBuf);
                        const char *valuePtr = JS_ToCString(ctx, jsvValue, &valueBuf);
                        if (!keyPtr || !valuePtr) continue;
                        String key = keyPtr;
                        String value = valuePtr;
                        options.push_back({key, [value, &result]() { result = value; }});
                    }
                }
            }
        }
    } else if (JS_IsObject(ctx, argv[0])) {
        /* element is expected to be {key: value} */
        log_d("element is expected to be {key: value}");
        uint32_t prop_count = 0;
        for (uint32_t index = 0;; ++index) {
            log_d("index: %d", index);
            const char *key = JS_GetOwnPropertyByIndex(ctx, index, &prop_count, argv[0]);
            if (key == NULL) break;
            log_d("key: %s", key);
            log_d("prop_count: %d", prop_count);
            String label = key;
            JSValue jsvVal = JS_GetPropertyStr(ctx, argv[0], key);
            if (JS_IsString(ctx, jsvVal)) {
                JSCStringBuf sb;
                const char *valPtr = JS_ToCString(ctx, jsvVal, &sb);
                if (!valPtr) continue;
                String value = valPtr;
                options.push_back({label, [value, &result]() { result = value; }});
            }
        }
    }

    loopOptions(options, MENU_TYPE_REGULAR, "", 0, true);
    options.clear();

    return JS_NewString(ctx, result.c_str());
}

JSValue native_dialogViewFile(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    if (argc < 1 || !JS_IsString(ctx, argv[0])) return JS_UNDEFINED;
    JSCStringBuf sb;
    const char *path = JS_ToCString(ctx, argv[0], &sb);
    String filepath = String(path);
    if (!filepath.startsWith("/")) filepath = "/" + filepath;
    FS *fs = NULL;
    if (SD.exists(filepath)) fs = &SD;
    else if (LittleFS.exists(filepath)) fs = &LittleFS; // ← add "else if" for SD Priority
    if (fs) { viewFile(*fs, filepath); }
    return JS_UNDEFINED;
}

JSValue native_dialogViewText(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    if (argc < 1 || !JS_IsString(ctx, argv[0])) return JS_UNDEFINED;
    tft.drawRoundRect(5, 5, tftWidth - 10, tftHeight - 10, 5, bruceConfig.priColor);
    uint8_t padY = 10;
    if (argc > 1 && JS_IsString(ctx, argv[1])) {
        JSCStringBuf sb;
        const char *title = JS_ToCString(ctx, argv[1], &sb);
        tft.setCursor((tftWidth - (strlen(title) * FM * LW)) / 2, padY);
        tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
        tft.setTextSize(FM);
        tft.println(title);
        padY = tft.getCursorY();
        tft.setTextSize(FP);
    }
    ScrollableTextArea area = ScrollableTextArea(
        1, 10, padY, tftWidth - 2 * BORDER_PAD_X, tftHeight - BORDER_PAD_X - padY, false, true
    );
    JSCStringBuf sb;
    const char *s = JS_ToCString(ctx, argv[0], &sb);
    area.fromString(s ? s : "");
    area.show(true);
    return JS_UNDEFINED;
}

static ScrollableTextArea *getAreaPointer(JSContext *ctx, JSValue obj) {
    if (!JS_IsObject(ctx, obj)) return NULL;
    if (JS_GetClassID(ctx, obj) != JS_CLASS_TEXTVIEWER) return NULL;
    TextViewerData *d = (TextViewerData *)JS_GetOpaque(ctx, obj);
    if (!d) return NULL;
    return d->area;
}

JSValue native_dialogCreateTextViewerDraw(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    ScrollableTextArea *area = getAreaPointer(ctx, *this_val);
    if (area == NULL) return JS_ThrowTypeError(ctx, "TextViewer: does not exist");
    area->draw(true);
    return JS_UNDEFINED;
}

JSValue native_dialogCreateTextViewerScrollUp(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    ScrollableTextArea *area = getAreaPointer(ctx, *this_val);
    if (area == NULL) return JS_ThrowTypeError(ctx, "TextViewer: does not exist");
    area->scrollUp();
    return JS_UNDEFINED;
}

JSValue native_dialogCreateTextViewerScrollDown(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    ScrollableTextArea *area = getAreaPointer(ctx, *this_val);
    if (area == NULL) return JS_ThrowTypeError(ctx, "TextViewer: does not exist");
    area->scrollDown();
    return JS_UNDEFINED;
}

JSValue
native_dialogCreateTextViewerScrollToLine(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    ScrollableTextArea *area = getAreaPointer(ctx, *this_val);
    if (area == NULL) return JS_ThrowTypeError(ctx, "TextViewer: does not exist");
    int ln = 0;
    if (argc > 0 && JS_IsNumber(ctx, argv[0])) JS_ToInt32(ctx, &ln, argv[0]);
    area->scrollToLine(ln);
    return JS_UNDEFINED;
}

JSValue native_dialogCreateTextViewerGetLine(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    ScrollableTextArea *area = getAreaPointer(ctx, *this_val);
    if (area == NULL) return JS_ThrowTypeError(ctx, "TextViewer: does not exist");
    int ln = 0;
    if (argc > 0 && JS_IsNumber(ctx, argv[0])) JS_ToInt32(ctx, &ln, argv[0]);
    return JS_NewString(ctx, area->getLine(ln).c_str());
}

JSValue native_dialogCreateTextViewerGetMaxLines(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    ScrollableTextArea *area = getAreaPointer(ctx, *this_val);
    if (area == NULL) return JS_ThrowTypeError(ctx, "TextViewer: does not exist");
    return JS_NewInt32(ctx, area->getMaxLines());
}

JSValue
native_dialogCreateTextViewerGetVisibleText(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    ScrollableTextArea *area = getAreaPointer(ctx, *this_val);
    if (area == NULL) return JS_ThrowTypeError(ctx, "TextViewer: does not exist");
    String visibleText;
    visibleText.reserve(area->getMaxVisibleTextLength());
    for (size_t i = area->firstVisibleLine; i < area->lastVisibleLine - 1; i++)
        visibleText += area->linesBuffer[i];
    return JS_NewString(ctx, visibleText.c_str());
}

JSValue native_dialogCreateTextViewerClear(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    ScrollableTextArea *area = getAreaPointer(ctx, *this_val);
    if (area == NULL) return JS_ThrowTypeError(ctx, "TextViewer: does not exist");
    area->clear();
    return JS_UNDEFINED;
}

JSValue native_dialogCreateTextViewerFromString(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    ScrollableTextArea *area = getAreaPointer(ctx, *this_val);
    if (area == NULL) return JS_ThrowTypeError(ctx, "TextViewer: does not exist");
    if (argc > 0 && JS_IsString(ctx, argv[0])) {
        JSCStringBuf sb;
        const char *s = JS_ToCString(ctx, argv[0], &sb);
        area->fromString(s ? s : "");
    }
    return JS_UNDEFINED;
}

JSValue native_dialogCreateTextViewerClose(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    JSValue obj = JS_UNDEFINED;
    if (argc > 0 && JS_IsObject(ctx, argv[0])) {
        obj = argv[0];
    } else if (this_val && JS_IsObject(ctx, *this_val)) {
        obj = *this_val;
    }

    if (!JS_IsObject(ctx, obj) || JS_GetClassID(ctx, obj) != JS_CLASS_TEXTVIEWER) return JS_UNDEFINED;

    void *opaque = JS_GetOpaque(ctx, obj);
    if (opaque) {
        native_textviewer_finalizer(ctx, opaque);
        JS_SetOpaque(ctx, obj, NULL);
    }
    return JS_UNDEFINED;
}

JSValue native_dialogCreateTextViewer(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    if (argc < 1 || !JS_IsString(ctx, argv[0])) return JS_ThrowTypeError(ctx, "TextViewer requires text");

    uint8_t fontSize = 1;
    int16_t startX = 10;
    int16_t startY = 10;
    int32_t width = tftWidth - 10;
    int32_t height = tftHeight - 10;
    bool indentWrappedLines = false;

    if (argc > 1 && JS_IsObject(ctx, argv[1])) {
        JSValue v;
        v = JS_GetPropertyStr(ctx, argv[1], "fontSize");
        if (!JS_IsUndefined(v) && JS_IsNumber(ctx, v)) {
            int tmp;
            JS_ToInt32(ctx, &tmp, v);
            fontSize = tmp;
        }
        v = JS_GetPropertyStr(ctx, argv[1], "startX");
        if (!JS_IsUndefined(v) && JS_IsNumber(ctx, v)) {
            int tmp;
            JS_ToInt32(ctx, &tmp, v);
            startX = tmp;
        }
        v = JS_GetPropertyStr(ctx, argv[1], "startY");
        if (!JS_IsUndefined(v) && JS_IsNumber(ctx, v)) {
            int tmp;
            JS_ToInt32(ctx, &tmp, v);
            startY = tmp;
        }
        v = JS_GetPropertyStr(ctx, argv[1], "width");
        if (!JS_IsUndefined(v) && JS_IsNumber(ctx, v)) {
            int tmp;
            JS_ToInt32(ctx, &tmp, v);
            width = tmp;
        }
        v = JS_GetPropertyStr(ctx, argv[1], "height");
        if (!JS_IsUndefined(v) && JS_IsNumber(ctx, v)) {
            int tmp;
            JS_ToInt32(ctx, &tmp, v);
            height = tmp;
        }
        v = JS_GetPropertyStr(ctx, argv[1], "indentWrappedLines");
        if (!JS_IsUndefined(v) && JS_IsBool(v)) indentWrappedLines = JS_ToBool(ctx, v);
    }

    ScrollableTextArea *area =
        new ScrollableTextArea(fontSize, startX, startY, width, height, false, indentWrappedLines);
    JSCStringBuf sb;
    const char *s = JS_ToCString(ctx, argv[0], &sb);
    area->fromString(s ? s : "");

    JSValue obj = JS_NewObjectClassUser(ctx, JS_CLASS_TEXTVIEWER);
    if (JS_IsException(obj)) {
        delete area;
        return obj;
    }

    TextViewerData *d = (TextViewerData *)malloc(sizeof(TextViewerData));
    if (!d) {
        delete area;
        return JS_ThrowOutOfMemory(ctx);
    }
    d->area = area;
    JS_SetOpaque(ctx, obj, d);

    return obj;
}

JSValue native_drawStatusBar(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
#if defined(HAS_SCREEN)
    drawStatusBar();
#endif
    return JS_UNDEFINED;
}

void native_textviewer_finalizer(JSContext *ctx, void *opaque) {
    TextViewerData *d = (TextViewerData *)opaque;
    if (!d) return;
    if (d->area) {
        delete d->area;
        d->area = NULL;
    }
    free(d);
}
#endif
