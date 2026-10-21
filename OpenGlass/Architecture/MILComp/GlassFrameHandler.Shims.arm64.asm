        AREA    |.text|, CODE, READONLY

        IMPORT  MyCLegacyNonClientBackground_HasSomethingToRender_Impl
        EXPORT  MyCLegacyNonClientBackground_HasSomethingToRender

; The x64 physical dispatcher preserves RCX and RDX because CloneVisualTreeForLivePreview
; is privately optimized across the original HasSomethingToRender call and reuses those
; volatile registers afterwards. On ARM64 the CLegacyNonClientBackground pointer arrives in
; X0, which is also the bool return register, so X0 cannot be restored without destroying
; the result. No ARM64 DWM binary for build 28000+ has been audited for an equivalent
; private contract, so this dispatcher keeps an ordinary relocatable frame and forwards the
; standard ABI result unchanged.
MyCLegacyNonClientBackground_HasSomethingToRender
        STP     X29, X30, [SP, #-32]!
        MOV     X29, SP

        BL      MyCLegacyNonClientBackground_HasSomethingToRender_Impl

        LDP     X29, X30, [SP], #32
        RET

        END
