#Requires AutoHotkey v2.0
#SingleInstance Force
Persistent
SendMode "Input"

; ======================= 配置区 =======================
CycleHotkey := "^!m"        ; 切换模式热键（AHK 语法：^=Ctrl  !=Alt  +=Shift  #=Win）
HotkeyName  := "Ctrl+Alt+M" ; 热键的显示名称（仅用于提示文字，改热键时同步改这里）

; ---- 提示框外观：全部按屏幕比例计算，自动适配任意分辨率 / 缩放 / 显示器 ----
; 换到别人的电脑上不需要改任何数值，启动时按对方显示器的工作区自动算
WidthRatio := 0.29          ; 宽度 = 显示器工作区宽度的 29%
WidthMin   := 300           ; 宽度下限（逻辑像素），防止小屏上过小
WidthMax   := 560           ; 宽度上限（逻辑像素），防止大屏上过大
HeightOf   := 0.19          ; 高度 = 宽度 × 0.19（约 5.25:1 的横条）
TopOf      := 0.06          ; 距工作区顶部 = 工作区高度的 6%
FontOf     := 0.225         ; 字号 = 高度 × 0.225

ToastAlpha := 160           ; 不透明度 0-255，越小越透明
ModeHoldMs := 1300          ; 模式提示自动消失时间(ms)
WatchMs    := 80            ; 按键松开的轮询间隔(ms)
FontName   := "Microsoft YaHei"
IconDir    := A_ScriptDir . "\..\icons"

; 打包成单文件 exe 时嵌入图标资源（Ahk2Exe 指令；直接跑脚本时是普通注释）
;@Ahk2Exe-AddResource ..\icons\normal.ico, 100
;@Ahk2Exe-AddResource ..\icons\num.ico,    101
;@Ahk2Exe-AddResource ..\icons\func.ico,   102
; ======================================================

; ---- 启动时按当前显示器算一次几何尺寸 ----
; 注意：用「工作区」而不是「全屏」，这样任务栏在顶部时也不会被遮挡；
;       X/Y 用物理像素、W/H 用逻辑像素（AHK 会自动按 DPI 放大 W/H）
Geo := ComputeGeometry()

ComputeGeometry() {
    global WidthRatio, WidthMin, WidthMax, HeightOf, TopOf, FontOf
    mon := MonitorGetPrimary()                      ; 多显示器时取主显示器
    MonitorGetWorkArea(mon, &wl, &wt, &wr, &wb)
    workW := wr - wl
    workH := wb - wt
    scale := A_ScreenDPI / 96                       ; 200% 缩放时 = 2.0
    lw := Min(Max(Round(workW / scale * WidthRatio), WidthMin), WidthMax)
    lh := Round(lw * HeightOf)
    x  := wl + (workW - Round(lw * scale)) // 2     ; 水平居中（物理像素）
    y  := wt + Round(workH * TopOf)                 ; 顶部按比例留白
    return {x: x, y: y, w: lw, h: lh, font: Round(lh * FontOf)}
}

Mode     := 1
ModeName := ["正常", "数字", "功能"]
ModeBack := ["1E1E1E", "0B3D91", "14532D"]

NumMap := Map("q","1","w","2","e","3","r","4","t","5","y","6","u","7","i","8","o","9","p","0")
FnMap  := Map("q","F1","w","F2","e","F3","r","F4","t","F5","y","F6","u","F7","i","F8","o","F9","p","F10","[","F11","]","F12")
ShiftSym := Map("1","!","2","@","3","#","4","$","5","%","6","^","7","&","8","*","9","(","0",")")

CurMap        := Map()
HeldKeys      := Map()
ToastOwner    := ""
AutoHideOwner := ""
ToastShown    := false

; ==================== 提示框 ====================
Toast := Gui("-Caption +AlwaysOnTop +ToolWindow +E0x20 +E0x08000000")
Toast.BackColor := "1E1E1E"
Toast.SetFont("s" . Geo.font . " Bold cWhite", FontName)
ToastText := Toast.AddText("x12 y10 w" . (Geo.w - 24) . " h" . (Geo.h - 20) . " Center +0x200 BackgroundTrans", "")
WinSetTransparent(ToastAlpha, Toast.Hwnd)

; 定位提示框。两个必须注意的点：
;  1) Gui.Show() 会覆盖之前 Move() 设定的位置（系统自动居中），所以必须先 Show 再 Move
;  2) AHK 会把 Move() 的 W/H 按 DPI 缩放、而 X/Y 不缩放，所以居中算式已在 ComputeGeometry 里算好
ToastPos() {
    global Toast, Geo
    Toast.Move(Geo.x, Geo.y, Geo.w, Geo.h)
}

; autoHideMs > 0 表示自动消失；= 0 表示保持到 HideToast 被调用
ShowToast(text, owner, autoHideMs) {
    global Toast, ToastText, ToastOwner, AutoHideOwner, ToastShown, ModeBack, Mode
    ; 键盘自动重复会高频触发，内容没变就跳过，避免反复 Show 造成闪动
    if (ToastShown && ToastOwner = owner && ToastText.Text = text)
        return
    ToastOwner := owner
    Toast.BackColor := ModeBack[Mode]
    ToastText.Text := text
    Toast.Show("NoActivate")
    ToastPos()          ; 必须在 Show 之后，否则位置会被 Show 覆盖
    ToastShown := true
    if (autoHideMs > 0) {
        AutoHideOwner := owner
        SetTimer(ToastAutoHide, -autoHideMs)
    }
}

ToastAutoHide() {
    global AutoHideOwner
    HideToast(AutoHideOwner)
}

; owner 为空表示强制隐藏；否则只隐藏属于该 owner 的提示
HideToast(owner) {
    global Toast, ToastOwner, ToastShown
    if (owner != "" && owner != ToastOwner)
        return
    ToastOwner := ""
    ToastShown := false
    SetTimer(ToastAutoHide, 0)
    Toast.Hide()
}

; 兜底看门狗：直接查询物理按键状态，不依赖 keyup 事件
; 即使键盘自动重复打乱了事件顺序，松手后提示也一定会消失
KeyWatch() {
    global ToastOwner, HeldKeys
    if (SubStr(ToastOwner, 1, 4) != "key:")
        return
    pk := SubStr(ToastOwner, 5)
    if !GetKeyState(pk, "P") {
        HeldKeys.Delete(pk)
        HideToast(ToastOwner)
    }
}

; ==================== 模式与热键 ====================
BuildModeMap(m) {
    global NumMap, FnMap
    out := Map()
    src := (m = 2) ? NumMap : (m = 3) ? FnMap : ""
    if (src = "")
        return out
    for k, v in src {
        out[k] := v
        out["+" . k] := v        ; Shift 变体走同一目标，物理 Shift 会自然产生 ! @ # 等
    }
    return out
}

ApplyMode() {
    global CurMap, HeldKeys, Mode
    for pk, t in HeldKeys
        Send "{Blind}{" . t . " up}"
    HeldKeys := Map()
    for hk, t in CurMap {
        try Hotkey(hk, "Off")
        try Hotkey(hk . " up", "Off")
    }
    CurMap := BuildModeMap(Mode)
    for hk, t in CurMap {
        try Hotkey(hk, OnKeyDown, "On")
        try Hotkey(hk . " up", OnKeyUp, "On")
    }
    SetTimer(KeyWatch, (Mode = 1) ? 0 : WatchMs)
    UpdateTray()
}

OnKeyDown(hk, *) {
    global CurMap, HeldKeys, ShiftSym
    if (Mode = 1 || !CurMap.Has(hk))
        return
    target := CurMap[hk]
    pk := (SubStr(hk, 1, 1) = "+") ? SubStr(hk, 2) : hk
    disp := target
    if (GetKeyState("Shift", "P") && ShiftSym.Has(target))
        disp := ShiftSym[target]
    ShowToast(StrUpper(pk) . "   →   " . disp, "key:" . pk, 0)
    Send "{Blind}{" . target . " down}"
    HeldKeys[pk] := target
}

OnKeyUp(hk, *) {
    global HeldKeys
    pk := (SubStr(hk, 1, 1) = "+") ? SubStr(hk, 2) : hk
    if !HeldKeys.Has(pk)
        return
    Send "{Blind}{" . HeldKeys[pk] . " up}"
    HeldKeys.Delete(pk)
    if (HeldKeys.Count = 0)
        HideToast("key:" . pk)
}

CycleMode(*) {
    global Mode
    Mode := Mod(Mode, 3) + 1
    ApplyMode()
    ShowToast("模式：" . ModeName[Mode], "mode", ModeHoldMs)
}

; ==================== 托盘 ====================
SetModeIcon() {
    global Mode, IconDir
    ; 优先用嵌入资源（打包成 exe 后走这条）；否则读同目录图标；再否则退回系统图标
    if A_IsCompiled {
        try {
            TraySetIcon(A_ScriptFullPath, -(99 + Mode))
            return
        }
    }
    files := ["normal.ico", "num.ico", "func.ico"]
    ico := IconDir . "\" . files[Mode]
    if FileExist(ico)
        TraySetIcon(ico)
    else
        TraySetIcon("shell32.dll", 21 + Mode)
}

UpdateTray(*) {
    global Mode, ModeName, HotkeyName
    SetModeIcon()
    A_IconTip := "按键映射 · " . ModeName[Mode] . "模式（" . HotkeyName . " 切换）"
    BuildTrayMenu()
}

BuildTrayMenu() {
    global Mode, ModeName, HotkeyName
    A_TrayMenu.Delete()
    A_TrayMenu.Add("当前模式：" . ModeName[Mode], (*) => CycleMode())
    item := "切换模式   (" . HotkeyName . ")"
    A_TrayMenu.Add(item, (*) => CycleMode())
    A_TrayMenu.Add()
    A_TrayMenu.Add("退出", (*) => ExitApp())
    A_TrayMenu.Default := item
    A_TrayMenu.ClickCount := 2
}

; ==================== 启动 ====================
Hotkey(CycleHotkey, (*) => CycleMode(), "On")
ApplyMode()
ShowToast("正常模式 · " . HotkeyName . " 切换", "mode", 2200)
