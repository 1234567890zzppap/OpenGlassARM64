        AREA    |.text|, CODE, READONLY

        IMPORT  MySetMargin_Impl
        EXPORT  MySetMargin

; The x64 physical dispatcher preserves RCX because UpdateMarginsDependentOnStyle
; privately reuses that argument register after the call. X0 cannot play that role on
; ARM64: it carries both the first argument and the bool return, and the 22631 ARM64
; caller reads W0 as the result (ubfm w20,w0,#0,#7 / tst / b.ne) and recomputes X0 from
; the window object before its next call. Restoring the incoming X0 here would overwrite
; the replacement's return value and hand UpdateMarginsDependentOnStyle a garbage bool,
; which drops the window shadow. Keep an ordinary frame so the detour target still has a
; relocatable prologue, and forward the ABI result unchanged.
MySetMargin
        STP     X29, X30, [SP, #-32]!
        MOV     X29, SP

        BL      MySetMargin_Impl

        LDP     X29, X30, [SP], #32
        RET

        END
