package com.example.msiscanner

import java.nio.ByteBuffer

enum class ChecksumMode(val nativeValue: Int, val label: String) {
    NONE(0, "No checksum"),
    MOD_10(1, "Mod 10"),
    MOD_11(2, "Mod 11"),
    MOD_1010(3, "Mod 1010"),
    MOD_1110(4, "Mod 1110"),
    AUTO(5, "Auto detect"),
}

data class MsiScanResult(
    val text: String,
    val checksumStatus: String,
    val meta: String,
)

object MsiNative {
    init {
        System.loadLibrary("msi_scanner")
    }

    private external fun nativeDecodeYPlane(
        y: ByteBuffer,
        offset: Int,
        width: Int,
        height: Int,
        rowStride: Int,
        checksumMode: Int,
        scanVertical: Boolean,
        minVotes: Int,
        minDigits: Int,
        maxDigits: Int,
    ): Array<String>?

    fun decode(
        y: ByteBuffer,
        offset: Int,
        width: Int,
        height: Int,
        rowStride: Int,
        mode: ChecksumMode,
        scanVertical: Boolean = true,
        minVotes: Int = 2,
        minDigits: Int = 4,
        maxDigits: Int = 12,
    ): MsiScanResult? {
        val raw = nativeDecodeYPlane(
            y, offset, width, height, rowStride, mode.nativeValue, scanVertical,
            minVotes, minDigits, maxDigits,
        ) ?: return null
        if (raw.size < 3) {
            return null
        }
        return MsiScanResult(raw[0], raw[1], raw[2])
    }
}
