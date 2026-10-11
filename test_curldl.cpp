// test_curldl.cpp - Kapsamlı Güvenlik ve İşlevsellik Test Paketi
#define CURLDL_NO_MAIN
#include "curldl.cpp"

#include <cassert>
#include <iostream>
#include <vector>
#include <string>

static int totalTests = 0;
static int passedTests = 0;

#define TEST_ASSERT(cond, msg) do { \
    totalTests++; \
    if (cond) { \
        passedTests++; \
        std::cout << "  [PASS] " << msg << "\n"; \
    } else { \
        std::cerr << "  [FAIL] " << msg << " (Satir: " << __LINE__ << ")\n"; \
    } \
} while(0)

void testUrlSchemeValidation() {
    std::cout << "[TEST SUITE] URL Sema Guvenlik ve Dogrulama Testleri:\n";

    // Gecerli URL'ler
    std::string u1 = "https://example.com/file.zip";
    TEST_ASSERT(validateAndNormalizeUrl(u1) && u1 == "https://example.com/file.zip", "HTTPS adresi kabul edilmeli");

    std::string u2 = "http://example.com/file.zip";
    TEST_ASSERT(validateAndNormalizeUrl(u2) && u2 == "http://example.com/file.zip", "HTTP adresi kabul edilmeli");

    std::string u3 = "example.com/file.zip";
    TEST_ASSERT(validateAndNormalizeUrl(u3) && u3 == "https://example.com/file.zip", "Semasiz adrese otomatik https eklenmeli");

    std::string u4 = "HTTP://EXAMPLE.COM/FILE.ZIP";
    TEST_ASSERT(validateAndNormalizeUrl(u4), "Buyuk harfli sema kabul edilmeli");

    // Tehlikeli / Yasakli Semalar (SEC-02 SSRF ve Yerel Dosya Erisimi Engeli)
    std::string bad1 = "file:///etc/passwd";
    TEST_ASSERT(!validateAndNormalizeUrl(bad1), "file:// semasi reddedilmeli");

    std::string bad2 = "gopher://127.0.0.1:6379/test";
    TEST_ASSERT(!validateAndNormalizeUrl(bad2), "gopher:// semasi reddedilmeli");

    std::string bad3 = "ftp://example.com/test.zip";
    TEST_ASSERT(!validateAndNormalizeUrl(bad3), "ftp:// semasi reddedilmeli");

    std::string bad4 = "data:text/plain;base64,SGVsbG8=";
    TEST_ASSERT(!validateAndNormalizeUrl(bad4), "data: semasi reddedilmeli");

    std::string bad5 = "javascript:alert(1)";
    TEST_ASSERT(!validateAndNormalizeUrl(bad5), "javascript: semasi reddedilmeli");
}

void testSanitization() {
    std::cout << "[TEST SUITE] Dosya Adi Guvenlik Temizleme (Sanitize) Testleri:\n";

    // Dizin Atlatma (Directory Traversal)
    TEST_ASSERT(sanitize("../../etc/passwd") == "passwd", "Dizin yollari temizlenmeli (../../)");
    TEST_ASSERT(sanitize("..\\..\\Windows\\System32\\cmd.exe") == "cmd.exe", "Ters bolu ile dizin yollari temizlenmeli");

    // Tehlikeli nokta degerleri
    TEST_ASSERT(sanitize(".") == "", "Tek nokta reddedilmeli");
    TEST_ASSERT(sanitize("..") == "", "Cift nokta reddedilmeli");
    TEST_ASSERT(sanitize("   ") == "", "Bosluk degeri reddedilmeli");

    // CLI Bayrak / Opsiyon Enjeksiyonu Engeli (SEC-06)
    TEST_ASSERT(sanitize("-rf") == "rf", "Bastaki tireler temizlenmeli (-rf -> rf)");
    TEST_ASSERT(sanitize("--output=hacked.txt") == "output=hacked.txt" || sanitize("--output=hacked.txt") == "", "Bastaki tireler kaldirilmali");
    TEST_ASSERT(sanitize("-v") == "v", "Tek tireli secenek temizlenmeli");

    // Tehlikeli Karakterler ve NTFS Stream Engeli (SEC-10)
    TEST_ASSERT(sanitize("file.txt:evil.exe") == "", "Iki nokta ust uste (:) reddedilmeli");
    TEST_ASSERT(sanitize("file*.txt") == "", "Yildiz (*) reddedilmeli");
    TEST_ASSERT(sanitize("file?.txt") == "", "Soru isareti (?) reddedilmeli");
    TEST_ASSERT(sanitize("file|pipe") == "", "Pipe (|) reddedilmeli");

    // Windows / FAT Ayrilmis Aygit Adlari Engeli
    TEST_ASSERT(sanitize("CON") == "", "Ayrilmis aygit adi CON reddedilmeli");
    TEST_ASSERT(sanitize("con.txt") == "", "Ayrilmis aygit adi con.txt reddedilmeli");
    TEST_ASSERT(sanitize("aux.tar.gz") == "", "Ayrilmis aygit adi aux.tar.gz reddedilmeli");
    TEST_ASSERT(sanitize("NUL") == "", "Ayrilmis aygit adi NUL reddedilmeli");
    TEST_ASSERT(sanitize("COM1") == "", "Ayrilmis aygit adi COM1 reddedilmeli");
}

void testContentDispositionParsing() {
    std::cout << "[TEST SUITE] Content-Disposition Baslik Ayrirma (RFC 5987 / RFC 6266) Testleri:\n";

    // RFC 5987 Dil Etiketli Format (SEC-07)
    std::string h1 = "Content-Disposition: attachment; filename*=UTF-8'en'quarterly_report.pdf";
    TEST_ASSERT(fromContentDisposition(h1) == "quarterly_report.pdf", "RFC 5987 dil etiketli (en) filename* ayrismali");

    std::string h2 = "Content-Disposition: attachment; filename*=UTF-8''document.zip";
    TEST_ASSERT(fromContentDisposition(h2) == "document.zip", "RFC 5987 dilsiz filename* ayrismali");

    // URL Encoded RFC 5987
    std::string h3 = "Content-Disposition: attachment; filename*=UTF-8''%54%65%73%74%20%46%69%6c%65.txt";
    TEST_ASSERT(fromContentDisposition(h3) == "Test File.txt", "RFC 5987 url-encoded ayrismali");

    // Standart filename cift tirnak ve ters bolu kacisi (SEC-08)
    std::string h4 = "Content-Disposition: attachment; filename=\"report\\\"2026\\\".pdf\"";
    TEST_ASSERT(fromContentDisposition(h4) == "report2026.pdf" || fromContentDisposition(h4) == "report\"2026\".pdf" || sanitize(fromContentDisposition(h4)).empty(),
                "Kacisli tirnaklar guvenle islenmeli");

    // filename* basarisiz oldugunda filename= alanina geri donus (Fallback)
    std::string h5 = "Content-Disposition: attachment; filename*=invalid_format; filename=\"backup.tar.gz\"";
    TEST_ASSERT(fromContentDisposition(h5) == "backup.tar.gz", "Gecersiz filename* durumunda standart filename'e geri donulmeli");
}

void testUrlFilenameExtraction() {
    std::cout << "[TEST SUITE] URL'den Dosya Adi Cikarma Testleri:\n";

    TEST_ASSERT(fromUrl("https://example.com/path/to/archive.tar.gz") == "archive.tar.gz", "Temiz URL yolu cikarmasi");
    TEST_ASSERT(fromUrl("https://example.com/path/to/photo%201.jpg?token=123#sec") == "photo 1.jpg", "URL decode ve query/fragment temizligi");
    TEST_ASSERT(fromUrl("https://example.com/archive.zip/") == "", "Sonda slash varsa bos donmeli");
    TEST_ASSERT(fromUrl("https://example.com/") == "", "Sadece host varsa bos donmeli");
}

int main() {
    std::cout << "========================================\n";
    std::cout << "curldl Guvenlik & Birim Testleri Basliyor\n";
    std::cout << "========================================\n";

    testUrlSchemeValidation();
    testSanitization();
    testContentDispositionParsing();
    testUrlFilenameExtraction();

    std::cout << "========================================\n";
    std::cout << "Sonuc: " << passedTests << " / " << totalTests << " test basarili.\n";
    std::cout << "========================================\n";

    return (passedTests == totalTests) ? 0 : 1;
}
