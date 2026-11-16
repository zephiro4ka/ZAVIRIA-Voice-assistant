package com.astrix.phone

import java.math.BigInteger
import java.security.MessageDigest
import java.security.SecureRandom

// SPAKE2 в группе RFC 3526 №14 — та же схема, что в ZAVIRIA/app/Spake2.cpp.
// Код с экрана компьютера не уходит в сеть; подслушавший обмен не может подбирать код у себя.
object Spake2 {
    private val P = BigInteger(
        "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74020BBEA63B139B22514A08798E3404DD" +
        "EF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED" +
        "EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3DC2007CB8A163BF0598DA48361C55D39A69163FA8FD24CF5F" +
        "83655D23DCA3AD961C62F356208552BB9ED529077096966D670C354E4ABC9804F1746C08CA18217C32905E462E36CE3B" +
        "E39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2BCBF6955817183995497CEA956AE515D2261898FA0510" +
        "15728E5A8AACAA68FFFFFFFFFFFFFFFF", 16)
    private val Q = P.subtract(BigInteger.ONE).shiftRight(1)
    private val G = BigInteger.valueOf(2)

    private fun sha(text: String) = MessageDigest.getInstance("SHA-256").digest(text.toByteArray())

    private fun element(label: String): BigInteger {
        val bytes = (0 until 8).flatMap { sha(label + it).toList() }.toByteArray()
        bytes[0] = (bytes[0].toInt() and 0x7F).toByte()
        val h = BigInteger(1, bytes)
        return h.multiply(h).mod(P)
    }

    private val M by lazy { element("ZAVIRIA-SPAKE2-M") }
    private val N by lazy { element("ZAVIRIA-SPAKE2-N") }

    private fun password(code: String) = BigInteger(1, sha("ZAVIRIA-SPAKE2-w$code"))
    private fun hex(n: BigInteger) = n.toString(16).padStart(512, '0')

    class Exchange(private val code: String) {
        private val x = BigInteger(1, ByteArray(32).also { SecureRandom().nextBytes(it) })
        val message: String = hex(G.modPow(x, P).multiply(M.modPow(password(code), P)).mod(P)) // X = g^x · M^w

        // Общий ключ (32 байта) или null, если ответ компьютера негодный
        fun finish(yHex: String): ByteArray? {
            val y = runCatching { BigInteger(yHex, 16) }.getOrNull() ?: return null
            if (y <= BigInteger.ONE || y >= P.subtract(BigInteger.ONE) || y.modPow(Q, P) != BigInteger.ONE) return null
            val k = y.multiply(N.modPow(Q.subtract(password(code)), P)).mod(P).modPow(x, P) // K = (Y · N^−w)^x
            return sha("ZAVIRIA-SPAKE2|$message|${hex(y)}|${hex(k)}")
        }
    }
}
