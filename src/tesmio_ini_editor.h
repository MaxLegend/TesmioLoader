#pragma once

namespace tsm
{
struct IniSetting
{
    std::wstring key;
    std::wstring comment;
    size_t begin;
    size_t end;
    HWND edit = nullptr;
};

struct IniEditor
{
    const std::wstring& path;
    HFONT font;
    int dpi;
    HWND viewport = nullptr;
    int rowHeight = 0;
    int scrollPos = 0;
    int wheelRemainder = 0;
    std::vector<IniSetting> settings;
    std::wstring text;
    std::string bytes;
    UINT codePage = CP_UTF8;
    bool utf8Bom = false;
    bool utf16 = false;

    bool Read()
    {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size;
        DWORD got = 0;
        bool ok = GetFileSizeEx(file, &size) && size.QuadPart <= 4 * 1024 * 1024;
        if (ok)
        {
            bytes.resize(static_cast<size_t>(size.QuadPart));
            ok = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr) &&
                 got == bytes.size();
        }
        CloseHandle(file);
        if (!ok) return false;

        utf16 = bytes.compare(0, 2, "\xFF\xFE") == 0;
        utf8Bom = bytes.compare(0, 3, "\xEF\xBB\xBF") == 0;
        if (utf16)
        {
            if (bytes.size() % sizeof(wchar_t)) return false;
            text.resize(bytes.size() / sizeof(wchar_t) - 1);
            memcpy(text.data(), bytes.data() + sizeof(wchar_t), text.size() * sizeof(wchar_t));
        }
        else
        {
            const int offset = utf8Bom ? 3 : 0;
            const int len = static_cast<int>(bytes.size()) - offset;
            if (len)
            {
                const char* start = bytes.data() + offset;
                int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, start, len, nullptr, 0);
                if (!count && !utf8Bom)
                {
                    codePage = GetACP();
                    count = MultiByteToWideChar(codePage, 0, start, len, nullptr, 0);
                }
                if (!count) return false;
                text.resize(count);
                if (!MultiByteToWideChar(codePage, 0, start, len, text.data(), count)) return false;
            }
        }
        return text.find(L'\0') == std::wstring::npos;
    }

    void ParseSettings()
    {
        std::wstring comment;
        const wchar_t* spaces = L" \t";
        for (size_t line = 0; line < text.size();)
        {
            const size_t newline = text.find_first_of(L"\r\n", line);
            const size_t end = newline == std::wstring::npos ? text.size() : newline;
            const size_t first = text.find_first_not_of(spaces, line);
            if (first < end)
            {
                if (text[first] == L';' || text[first] == L'#')
                {
                    size_t content = first + 1;
                    if (content < end && text[content] == L' ') ++content;
                    if (!comment.empty()) comment += L"\r\n";
                    comment.append(text, content, end - content);
                }
                else if (text[first] == L'[')
                {
                    comment.clear();
                }
                else
                {
                    const size_t equals = text.find(L'=', first);
                    if (equals < end && equals > first)
                    {
                        const size_t keyEnd = text.find_last_not_of(spaces, equals - 1) + 1;
                        const size_t begin = (std::min)(text.find_first_not_of(spaces, equals + 1), end);
                        size_t valueEnd = end;
                        while (valueEnd > begin && (text[valueEnd - 1] == L' ' || text[valueEnd - 1] == L'\t'))
                            --valueEnd;
                        settings.push_back({text.substr(first, keyEnd - first),
                                            std::move(comment), begin, valueEnd});
                    }
                    comment.clear();
                }
            }
            else
                comment.clear();

            line = end + 1;
            if (newline != std::wstring::npos && text[end] == L'\r' &&
                line < text.size() && text[line] == L'\n')
                ++line;
        }
    }

    bool Save()
    {
        std::wstring value;
        size_t last = 0;
        for (const auto& setting : settings)
        {
            value.append(text, last, setting.begin - last);
            const int length = GetWindowTextLengthW(setting.edit);
            std::wstring edited(static_cast<size_t>(length) + 1, L'\0');
            edited.resize(GetWindowTextW(setting.edit, edited.data(), length + 1));
            value += edited;
            last = setting.end;
        }
        value.append(text, last, std::wstring::npos);
        if (value == text) return true;

        std::string encoded;
        if (utf16)
        {
            encoded = "\xFF\xFE";
            encoded.append(reinterpret_cast<const char*>(value.data()), value.size() * sizeof(wchar_t));
        }
        else
        {
            if (utf8Bom) encoded = "\xEF\xBB\xBF";
            const int offset = static_cast<int>(encoded.size());
            if (!value.empty())
            {
                const int count = WideCharToMultiByte(codePage, 0, value.data(),
                    static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
                if (!count) return false;
                encoded.resize(offset + count);
                BOOL substituted = FALSE;
                if (!WideCharToMultiByte(codePage, codePage == CP_UTF8 ? 0 : WC_NO_BEST_FIT_CHARS,
                        value.data(), static_cast<int>(value.size()), encoded.data() + offset, count,
                        nullptr, codePage == CP_UTF8 ? nullptr : &substituted) || substituted)
                    return false;
            }
        }

        // Steam can replace a Workshop file while the editor is open.
        IniEditor current{path, font, dpi};
        if (!current.Read() || current.bytes != bytes) return false;
        wchar_t temp[MAX_PATH];
        if (!GetTempFileNameW(Parent(path).c_str(), L"tsm", 0, temp)) return false;
        HANDLE file = CreateFileW(temp, GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD written = 0;
        const bool ok = file != INVALID_HANDLE_VALUE &&
            WriteFile(file, encoded.data(), static_cast<DWORD>(encoded.size()), &written, nullptr) &&
            written == encoded.size() && FlushFileBuffers(file);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        if (ok && MoveFileExW(temp, path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return true;
        DeleteFileW(temp);
        return false;
    }
};

inline void ScrollIniForm(IniEditor& editor, int requested)
{
    SCROLLINFO bar = {sizeof(bar), SIF_POS};
    bar.nPos = requested;
    const int position = SetScrollInfo(editor.viewport, SB_VERT, &bar, TRUE);
    if (position == editor.scrollPos) return;
    const int delta = editor.scrollPos - position;
    for (HWND child = GetWindow(editor.viewport, GW_CHILD); child;
         child = GetWindow(child, GW_HWNDNEXT))
    {
        RECT bounds;
        GetWindowRect(child, &bounds);
        MapWindowPoints(nullptr, editor.viewport, reinterpret_cast<POINT*>(&bounds), 2);
        SetWindowPos(child, nullptr, bounds.left, bounds.top + delta, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
    }
    editor.scrollPos = position;
    RedrawWindow(editor.viewport, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

inline LRESULT CALLBACK IniViewportProc(HWND window, UINT msg, WPARAM wp, LPARAM lp,
                                         UINT_PTR subclass, DWORD_PTR context)
{
    auto& editor = *reinterpret_cast<IniEditor*>(context);
    switch (msg)
    {
    case WM_VSCROLL:
    {
        SCROLLINFO bar = {sizeof(bar), SIF_PAGE | SIF_TRACKPOS | SIF_RANGE};
        GetScrollInfo(window, SB_VERT, &bar);
        int position = editor.scrollPos;
        switch (LOWORD(wp))
        {
        case SB_TOP: position = 0; break;
        case SB_BOTTOM: position = bar.nMax; break;
        case SB_LINEUP: position -= editor.rowHeight; break;
        case SB_LINEDOWN: position += editor.rowHeight; break;
        case SB_PAGEUP: position -= static_cast<int>(bar.nPage); break;
        case SB_PAGEDOWN: position += static_cast<int>(bar.nPage); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: position = bar.nTrackPos; break;
        }
        ScrollIniForm(editor, position);
        return 0;
    }
    case WM_MOUSEWHEEL:
    {
        editor.wheelRemainder += GET_WHEEL_DELTA_WPARAM(wp);
        const int steps = editor.wheelRemainder / WHEEL_DELTA;
        editor.wheelRemainder %= WHEEL_DELTA;
        if (steps) ScrollIniForm(editor, editor.scrollPos - steps * 3 * editor.rowHeight);
        return 0;
    }
    case WM_COMMAND:
        if (HIWORD(wp) == EN_SETFOCUS)
        {
            RECT field, view;
            GetWindowRect(reinterpret_cast<HWND>(lp), &field);
            MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&field), 2);
            GetClientRect(window, &view);
            if (field.top < view.top)
                ScrollIniForm(editor, editor.scrollPos + field.top);
            else if (field.bottom > view.bottom)
                ScrollIniForm(editor, editor.scrollPos + field.bottom - view.bottom);
        }
        return 0;
    case WM_NCDESTROY:
        RemoveWindowSubclass(window, IniViewportProc, subclass);
        break;
    }
    return DefSubclassProc(window, msg, wp, lp);
}

inline HWND IniControl(HWND parent, HFONT font, const wchar_t* cls, const wchar_t* label,
                       DWORD style, int x, int y, int w, int h, int id = -1)
{
    HWND child = CreateWindowExW(0, cls, label, WS_CHILD | WS_VISIBLE | style, x, y, w, h,
                                 parent, (HMENU)(INT_PTR)id, nullptr, nullptr);
    if (child) SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
    return child;
}

inline bool PopulateIniForm(IniEditor& editor, HWND dialog, int pad, int width, int height)
{
    editor.viewport = CreateWindowExW(WS_EX_CONTROLPARENT, L"STATIC", L"",
                                       WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPCHILDREN,
                                       pad, pad, width, height, dialog, nullptr, nullptr, nullptr);
    if (!editor.viewport ||
        !SetWindowSubclass(editor.viewport, IniViewportProc, 1, reinterpret_cast<DWORD_PTR>(&editor)))
        return false;

    const auto scale = [&](int n) { return MulDiv(n, editor.dpi, 96); };
    editor.rowHeight = scale(29);
    SCROLLINFO bar = {sizeof(bar), SIF_RANGE | SIF_PAGE | SIF_POS};
    bar.nMax = editor.settings.empty() ? 0 :
        static_cast<int>(editor.settings.size()) * editor.rowHeight - 1;
    bar.nPage = height;
    SetScrollInfo(editor.viewport, SB_VERT, &bar, TRUE);

    HWND tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                   WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                   0, 0, 0, 0, dialog, nullptr, nullptr, nullptr);
    if (tooltip) SendMessageW(tooltip, TTM_SETMAXTIPWIDTH, 0, scale(390));

    RECT client;
    GetClientRect(editor.viewport, &client);
    const int gap = scale(8);
    const int keyW = client.right * 44 / 100;
    const int helpW = scale(25);
    const int valueX = keyW + gap;
    const int helpX = client.right - helpW;
    const int valueW = helpX - gap - valueX;
    const int fieldH = scale(23);
    int y = 0;
    for (auto& setting : editor.settings)
    {
        IniControl(editor.viewport, editor.font, L"STATIC", setting.key.c_str(),
                   SS_NOPREFIX | SS_CENTERIMAGE | SS_ENDELLIPSIS,
                   0, y, keyW, fieldH);
        const std::wstring initial = editor.text.substr(setting.begin, setting.end - setting.begin);
        setting.edit = IniControl(editor.viewport, editor.font, L"EDIT", initial.c_str(),
                                  WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
                                  valueX, y, valueW, fieldH);
        if (!setting.edit) return false;
        SendMessageW(setting.edit, EM_SETLIMITTEXT, 4 * 1024 * 1024, 0);

        if (!setting.comment.empty())
        {
            HWND help = IniControl(editor.viewport, editor.font, L"STATIC", L"(?)",
                                   SS_CENTER | SS_CENTERIMAGE | SS_NOTIFY, helpX, y, helpW, fieldH);
            if (help && tooltip)
            {
                TOOLINFOW tool = {};
                tool.cbSize = sizeof(tool);
                tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
                tool.hwnd = editor.viewport;
                tool.uId = reinterpret_cast<UINT_PTR>(help);
                tool.lpszText = setting.comment.data();
                SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
            }
        }
        y += editor.rowHeight;
    }
    return true;
}

inline INT_PTR CALLBACK IniEditorProc(HWND dialog, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_INITDIALOG)
    {
        auto& editor = *reinterpret_cast<IniEditor*>(lp);
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        const auto leaf = editor.path.find_last_of(L'\\') + 1;
        SetWindowTextW(dialog, (L"Settings - " + editor.path.substr(leaf)).c_str());
        const auto scale = [&](int n) { return MulDiv(n, editor.dpi, 96); };
        const int pad = scale(12);
        const int buttonW = scale(86);
        const int buttonH = scale(26);

        MONITORINFO monitor = {sizeof(monitor)};
        if (GetMonitorInfoW(MonitorFromWindow(dialog, MONITOR_DEFAULTTONEAREST), &monitor))
        {
            const RECT work = monitor.rcWork;
            const int width = (std::min)(scale(405), static_cast<int>(work.right - work.left));
            const int height = (std::min)(scale(540), static_cast<int>(work.bottom - work.top));
            SetWindowPos(dialog, nullptr, work.left + (work.right - work.left - width) / 2,
                         work.top + (work.bottom - work.top - height) / 2,
                         width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        }

        RECT client;
        GetClientRect(dialog, &client);
        const int buttonY = client.bottom - pad - buttonH;
        if (!PopulateIniForm(editor, dialog, pad, client.right - 2 * pad, buttonY - 2 * pad))
        {
            EndDialog(dialog, -1);
            return TRUE;
        }
        IniControl(dialog, editor.font, L"BUTTON", L"Save", WS_TABSTOP | BS_DEFPUSHBUTTON,
                   client.right - 2 * (pad + buttonW), buttonY, buttonW, buttonH, IDOK);
        IniControl(dialog, editor.font, L"BUTTON", L"Cancel", WS_TABSTOP,
                   client.right - pad - buttonW, buttonY, buttonW, buttonH, IDCANCEL);
        if (!editor.settings.empty())
        {
            SetFocus(editor.settings.front().edit);
            return FALSE;
        }
        return TRUE;
    }
    if (msg == WM_MOUSEWHEEL)
    {
        const auto* editor = reinterpret_cast<const IniEditor*>(GetWindowLongPtrW(dialog, DWLP_USER));
        if (editor && editor->viewport)
            SendMessageW(editor->viewport, msg, wp, lp);
        return TRUE;
    }
    if (msg == WM_CLOSE || (msg == WM_COMMAND && LOWORD(wp) == IDCANCEL))
    {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    if (msg == WM_COMMAND && LOWORD(wp) == IDOK)
    {
        auto& editor = *reinterpret_cast<IniEditor*>(GetWindowLongPtrW(dialog, DWLP_USER));
        if (editor.Save())
            EndDialog(dialog, IDOK);
        else
            MessageBoxW(dialog, L"Could not save this INI. Check its encoding, permissions, or whether Steam updated it.",
                        L"TesmioLoader", MB_OK | MB_ICONERROR);
        return TRUE;
    }
    return FALSE;
}

inline void EditPluginIni(HWND owner, const std::wstring& path, HFONT font, int dpi)
{
    IniEditor editor{path, font, dpi};
    if (!editor.Read())
    {
        MessageBoxW(owner, L"Could not read this INI.", L"TesmioLoader", MB_OK | MB_ICONERROR);
        return;
    }

    editor.ParseSettings();

    // A dialog template with no resource ID keeps the editor in the launcher
    // even in a build without rc.exe. The controls are created on WM_INITDIALOG.
    struct Template
    {
        DLGTEMPLATE dialog;
        WORD menu, windowClass, title;
    };
    alignas(DWORD) const Template layout = {
        {WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER,
         0, 0, 0, 0, 205, 270}, 0, 0, 0
    };
    if (DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &layout.dialog, owner, IniEditorProc,
                                reinterpret_cast<LPARAM>(&editor)) == -1)
        MessageBoxW(owner, L"Could not open the INI editor.", L"TesmioLoader", MB_OK | MB_ICONERROR);
}
}
