package chat;

import javax.crypto.Cipher;
import javax.crypto.SecretKey;
import javax.crypto.SecretKeyFactory;
import javax.crypto.spec.GCMParameterSpec;
import javax.crypto.spec.PBEKeySpec;
import javax.crypto.spec.SecretKeySpec;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.StandardCharsets;
import java.security.GeneralSecurityException;
import java.security.SecureRandom;
import java.security.spec.KeySpec;
import java.util.Base64;

public final class Crypto {
    public static final String SALT = "NetworkChatLab3_Salt";
    public static final int PBKDF2_ITERATIONS = 100_000;
    public static final int KEY_LENGTH_BITS = 256;
    public static final int IV_LENGTH_BYTES = 12;
    public static final int GCM_TAG_LENGTH_BITS = 128;
    public static final int GCM_TAG_LENGTH_BYTES = GCM_TAG_LENGTH_BITS / 8;

    private static final String KDF_ALGORITHM = "PBKDF2WithHmacSHA256";
    private static final String CIPHER_ALGORITHM = "AES/GCM/NoPadding";

    private final SecretKey key;
    private final SecureRandom secureRandom = new SecureRandom();

    public record EncryptedPayload(String payloadBase64, String ivBase64) {}

    public static final class CryptoException extends Exception {
        public CryptoException(String message, Throwable cause) {
            super(message, cause);
        }

        public CryptoException(String message) {
            super(message);
        }
    }

    public Crypto(String passphrase) {
        if (passphrase == null || passphrase.isEmpty()) {
            throw new IllegalArgumentException("Passphrase must not be empty");
        }
        this.key = deriveKey(passphrase);
    }

    private static SecretKey deriveKey(String passphrase) {
        try {
            SecretKeyFactory factory = SecretKeyFactory.getInstance(KDF_ALGORITHM);
            KeySpec spec = new PBEKeySpec(
                    passphrase.toCharArray(),
                    SALT.getBytes(StandardCharsets.UTF_8),
                    PBKDF2_ITERATIONS,
                    KEY_LENGTH_BITS
            );
            byte[] keyBytes = factory.generateSecret(spec).getEncoded();
            return new SecretKeySpec(keyBytes, "AES");
        } catch (GeneralSecurityException e) {
            throw new IllegalStateException("Failed to derive AES-256 key via PBKDF2-HMAC-SHA256", e);
        }
    }

    public EncryptedPayload encrypt(String plaintext) throws CryptoException {
        try {
            byte[] iv = new byte[IV_LENGTH_BYTES];
            secureRandom.nextBytes(iv);

            Cipher cipher = Cipher.getInstance(CIPHER_ALGORITHM);
            cipher.init(Cipher.ENCRYPT_MODE, key, new GCMParameterSpec(GCM_TAG_LENGTH_BITS, iv));

            // SunJCE AES/GCM/NoPadding outputs ciphertext || 16-byte authentication tag.
            byte[] ciphertextAndTag = cipher.doFinal(plaintext.getBytes(StandardCharsets.UTF_8));

            return new EncryptedPayload(
                    Base64.getEncoder().encodeToString(ciphertextAndTag),
                    Base64.getEncoder().encodeToString(iv)
            );
        } catch (GeneralSecurityException e) {
            throw new CryptoException("Encryption failed", e);
        }
    }

    public String decrypt(String payloadBase64, String ivBase64) throws CryptoException {
        byte[] ciphertextAndTag;
        byte[] iv;
        try {
            ciphertextAndTag = Base64.getDecoder().decode(payloadBase64);
            iv = Base64.getDecoder().decode(ivBase64);
        } catch (IllegalArgumentException e) {
            throw new CryptoException("invalid_base64", e);
        }

        if (iv.length != IV_LENGTH_BYTES) {
            throw new CryptoException("invalid_iv_length (" + iv.length + ")");
        }
        if (ciphertextAndTag.length < GCM_TAG_LENGTH_BYTES) {
            throw new CryptoException("payload_too_short (" + ciphertextAndTag.length + ")");
        }

        try {
            Cipher cipher = Cipher.getInstance(CIPHER_ALGORITHM);
            cipher.init(Cipher.DECRYPT_MODE, key, new GCMParameterSpec(GCM_TAG_LENGTH_BITS, iv));
            byte[] plaintextBytes = cipher.doFinal(ciphertextAndTag);
            return Protocol.decodeUtf8Strict(plaintextBytes, plaintextBytes.length);
        } catch (CharacterCodingException e) {
            throw new CryptoException("invalid_utf8_plaintext", e);
        } catch (GeneralSecurityException e) {
            throw new CryptoException("auth_tag_or_key_mismatch", e);
        }
    }
}
