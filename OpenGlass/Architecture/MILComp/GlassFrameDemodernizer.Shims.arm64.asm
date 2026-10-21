        AREA    |.text|, CODE, READONLY

        IMPORT  MySetMargin_Impl
        EXPORT  MySetMargin

; The x64 physical dispatcher preserves RCX because UpdateMarginsDependentOnStyle
; privately reuses that argument register after the call. X0 cannot play that role on
; ARM64: it carries both the first argument and the bool return, so restoring it would
; overwrite the replacement's result. No ARM64 DWM binary for build 28000+ has been
; audited for an equivalent private contract, so this dispatcher keeps an ordinary
; relocatable frame and forwards the standard ABI result unchanged.
MySetMargin
        STP     X29, X30, [SP, #-32]!
        MOV     X29, SP

        BL      MySetMargin_Impl

        LDP     X29, X30, [SP], #32
        RET

        END
