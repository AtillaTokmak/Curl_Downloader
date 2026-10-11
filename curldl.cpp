// curldl - curl ile indirmeyi kısaltır, dosya adını URL'den kendisi bulur.
// Kullanım: curldl <url> [-o dosya_adi]
// Tek dosyalı, bağımlılıksız saf C++ scripti (sistem curl komutunu çağırır)
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

// --- Yardımcılar ---

static const size_t kMaxNameBytes = 255;           // NAME_MAX (Linux)
static const size_t kMaxProbeBytes = 1024 * 1024;  // probe çıktısı üst sınırı

static int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static std::string urlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && hexVal(s[i + 1]) >= 0 && hexVal(s[i + 2]) >= 0) {
            out += static_cast<char>(hexVal(s[i + 1]) * 16 + hexVal(s[i + 2]));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

static std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

static std::string toLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Terminale yazılacak her şey buradan geçer: yazdırılamayan baytlar \xNN olur
// (ANSI/C1 escape dizileriyle terminal manipülasyonunu engeller).
static std::string esc(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (c >= 0x20 && c < 0x7F && c != '\\') {
            out += static_cast<char>(c);
        } else {
            out += "\\x";
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

// Geçerli UTF-8 mi? (C1 kontrol baytlarını ve bozuk dizileri dışarıda bırakır)
static bool validUtf8(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len;
        if (c < 0x80) { ++i; continue; }
        else if ((c & 0xE0) == 0xC0 && c >= 0xC2) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0 && c <= 0xF4) len = 4;
        else return false;
        if (i + len > s.size()) return false;
        for (size_t k = 1; k < len; ++k)
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        // U+0080..U+009F (C1 kontrolleri) -> C2 80..C2 9F
        unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
        if (len == 2 && c == 0xC2 && c1 < 0xA0) return false;
        if (c == 0xE0 && c1 < 0xA0) return false;   // overlong 3 bayt
        if (c == 0xED && c1 > 0x9F) return false;   // UTF-16 surrogate
        if (c == 0xF0 && c1 < 0x90) return false;   // overlong 4 bayt
        if (c == 0xF4 && c1 > 0x8F) return false;   // > U+10FFFF
        i += len;
    }
    return true;
}

// --- Dosya Adı Çıkarma ve Temizleme Fonksiyonları ---

// Ayrılmış aygıt adları (CON, PRN, AUX, NUL, COM1-9, LPT1-9) - bağlı FAT/NTFS diskleri için
static bool isReservedDeviceName(const std::string& name) {
    if (name.empty()) return false;
    std::string stem = name.substr(0, name.find('.'));
    for (char& c : stem) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL") return true;
    if (stem.size() == 4 && (stem.rfind("COM", 0) == 0 || stem.rfind("LPT", 0) == 0) &&
        stem[3] >= '1' && stem[3] <= '9')
        return true;
    return false;
}

// Yol bileşenlerini atar (../../etc/passwd -> passwd), tehlikeli adları reddeder.
// autoName=true: ad sunucudan/URL'den geldi -> baştaki noktalar da silinir (.bashrc -> bashrc).
static std::string sanitize(const std::string& name, bool autoName) {
    std::string base = name;
    size_t slash = base.find_last_of("/\\");
    if (slash != std::string::npos) base = base.substr(slash + 1);

    for (char c : base) {
        if (c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || c == '\0')
            return "";
    }

    std::string out;
    for (char c : base)
        if (static_cast<unsigned char>(c) >= 32 && c != 127) out += c;

    if (!validUtf8(out)) return "";

    out = trim(out);
    // CLI opsiyon enjeksiyonu (-rf) ve otomatik gizli dosya (.bashrc) engeli
    while (!out.empty() && (out.front() == '-' || (autoName && out.front() == '.')))
        out.erase(0, 1);
    out = trim(out);

    if (out.empty() || out == "." || out == "..") return "";
    if (isReservedDeviceName(out)) return "";
    if (out.size() > kMaxNameBytes) return "";
    return out;
}

static bool hasExtension(const std::string& n) {
    size_t dot = n.rfind('.');
    return dot != std::string::npos && dot > 0 && dot + 1 < n.size();
}

// URL yolunun son parçasını döner.
static std::string fromUrl(const std::string& url) {
    std::string u = url.substr(0, url.find('#'));
    u = u.substr(0, u.find('?'));

    size_t scheme = u.find("://");
    if (scheme != std::string::npos) u = u.substr(scheme + 3);

    size_t firstSlash = u.find('/');
    if (firstSlash == std::string::npos) return "";  // sadece host
    std::string path = u.substr(firstSlash);
    if (path.back() == '/') return "";

    return sanitize(urlDecode(path.substr(path.rfind('/') + 1)), true);
}

// "name=" parametresini, başka bir parametrenin parçası olmayacak şekilde bulur.
static size_t findParam(const std::string& lower, const std::string& key) {
    size_t cur = 0;
    while (true) {
        size_t found = lower.find(key, cur);
        if (found == std::string::npos) return found;
        if (found == 0 || lower[found - 1] == ';' ||
            std::isspace(static_cast<unsigned char>(lower[found - 1])))
            return found;
        cur = found + key.size();
    }
}

// Content-Disposition başlığından ad çıkarır (RFC 5987 / RFC 6266)
static std::string fromContentDisposition(const std::string& header) {
    std::string lower = toLower(header);

    size_t p = findParam(lower, "filename*=");
    if (p != std::string::npos) {
        std::string v = header.substr(p + 10);
        v = trim(v.substr(0, v.find(';')));
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        size_t q1 = v.find('\'');
        size_t q2 = (q1 == std::string::npos) ? std::string::npos : v.find('\'', q1 + 1);
        if (q2 != std::string::npos) {
            std::string candidate = sanitize(urlDecode(v.substr(q2 + 1)), true);
            if (!candidate.empty()) return candidate;
        }
        // geçersizse filename= alanına düş
    }

    p = findParam(lower, "filename=");
    if (p == std::string::npos) return "";
    std::string v = trim(header.substr(p + 9));
    std::string raw;
    if (!v.empty() && v[0] == '"') {
        bool escape = false;
        for (size_t i = 1; i < v.size(); ++i) {
            if (escape) { raw += v[i]; escape = false; }
            else if (v[i] == '\\') escape = true;
            else if (v[i] == '"') break;
            else raw += v[i];
        }
    } else {
        raw = trim(v.substr(0, v.find(';')));
    }
    return sanitize(raw, true);
}

// "userinfo@host:port" biçiminde makul bir otorite mi? (IPv6 [..] destekli)
static bool looksLikeAuthority(const std::string& auth) {
    std::string a = auth;
    size_t at = a.rfind('@');
    if (at != std::string::npos) a = a.substr(at + 1);
    if (a.empty()) return false;

    std::string port;
    if (a[0] == '[') {
        size_t close = a.find(']');
        if (close == std::string::npos || close == 1) return false;
        for (size_t i = 1; i < close; ++i) {
            char c = a[i];
            if (!std::isxdigit(static_cast<unsigned char>(c)) && c != ':' && c != '.') return false;
        }
        std::string rest = a.substr(close + 1);
        if (!rest.empty()) {
            if (rest[0] != ':') return false;
            port = rest.substr(1);
            if (port.empty()) return false;
        }
    } else {
        size_t colon = a.find(':');
        std::string host = a.substr(0, colon);
        if (host.empty()) return false;
        for (char c : host) {
            unsigned char u = static_cast<unsigned char>(c);
            if (!std::isalnum(u) && c != '.' && c != '-' && c != '_' && u < 0x80) return false;
        }
        if (colon != std::string::npos) {
            port = a.substr(colon + 1);
            if (port.empty()) return false;
        }
    }
    for (char c : port)
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

// URL'yi doğrular/normalize eder. Yalnızca http ve https; kontrol karakteri yok.
static bool validateAndNormalizeUrl(std::string& url) {
    if (url.empty()) return false;
    for (unsigned char c : url)
        if (c <= 0x20 || c == 0x7F) return false;  // CR/LF/TAB/boşluk/kontrol

    size_t sep = url.find("://");
    if (sep != std::string::npos) {
        std::string scheme = toLower(url.substr(0, sep));
        if (scheme != "http" && scheme != "https") return false;
        std::string rest = url.substr(sep + 3);
        std::string auth = rest.substr(0, rest.find_first_of("/?#"));
        return looksLikeAuthority(auth);
    }

    // Şema yok: başı host gibi görünüyorsa https:// ekle, değilse (javascript:, data:, file:) reddet
    std::string auth = url.substr(0, url.find_first_of("/?#"));
    if (!looksLikeAuthority(auth)) return false;
    url = "https://" + url;
    return true;
}

// --- Süreç Yönetimi ---

static volatile sig_atomic_t g_signal = 0;
static void onSignal(int sig) { g_signal = sig; }

static void installSignalHandlers() {
    struct sigaction sa{};
    sa.sa_handler = onSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  // SA_RESTART yok: waitpid EINTR ile döner
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGHUP, &sa, nullptr);
}

static void resetSignalHandlersInChild() {
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
}

static void execCurl(const std::vector<std::string>& argv) {
    std::vector<char*> args;
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    execvp("curl", args.data());
}

// Sinyalleri çocuğa ileterek bekler. EINTR'ye güvenmez (fork ile waitpid arasındaki
// sinyal kaçmasın diye 50 ms aralıkla yoklar). Dönüş: çocuk toplandıysa true.
static bool waitChild(pid_t pid, int& status) {
    bool forwarded = false;
    while (true) {
        if (g_signal && !forwarded) {
            kill(pid, g_signal);
            forwarded = true;
        }
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) return true;
        if (r < 0 && errno != EINTR) return false;
        usleep(50 * 1000);
    }
}

// Son yanıt bloğundaki (son "HTTP/" satırından sonraki) Content-Disposition
static std::string finalContentDisposition(const std::string& headers) {
    std::string found;
    size_t pos = 0;
    while (pos < headers.size()) {
        size_t eol = headers.find('\n', pos);
        if (eol == std::string::npos) eol = headers.size();
        std::string line = headers.substr(pos, eol - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.compare(0, 5, "HTTP/") == 0) {
            found.clear();  // yeni yanıt bloğu: ara yönlendirmelerin başlıklarını unut
        } else if (line.size() > 20 && toLower(line.substr(0, 20)) == "content-disposition:") {
            found = line;
        }
        pos = eol + 1;
    }
    return found;
}

// HEAD istekleriyle başlıkları ve son URL'yi alır (kabuk kullanmadan).
static bool probe(const std::string& url, std::string& headers, std::string& finalUrl, int& httpCode) {
    int pipefd[2];
    if (pipe(pipefd) < 0) return false;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }
    if (pid == 0) {
        resetSignalHandlersInChild();
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) _exit(127);
        close(pipefd[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execCurl({"curl", "-q", "-sIL",
                  "--proto", "=http,https", "--proto-redir", "=http,https",
                  "--max-redirs", "10", "--max-time", "15",
                  "-w", "\n%{http_code} %{url_effective}\n",
                  "--url", url});
        _exit(127);
    }

    close(pipefd[1]);
    std::string all;
    char buf[4096];
    bool tooBig = false;
    while (!g_signal) {
        struct pollfd pfd{};
        pfd.fd = pipefd[0];
        pfd.events = POLLIN;
        int pr = poll(&pfd, 1, 200);  // 200 ms: sinyal kontrolü için
        if (pr < 0 && errno == EINTR) continue;
        if (pr < 0) break;
        if (pr == 0) continue;
        ssize_t n = read(pipefd[0], buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        all.append(buf, static_cast<size_t>(n));
        if (all.size() > kMaxProbeBytes) { tooBig = true; break; }
    }
    close(pipefd[0]);
    if (tooBig || g_signal) kill(pid, SIGKILL);

    int status = 0;
    if (!waitChild(pid, status)) return false;
    if (tooBig || g_signal) return false;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return false;

    size_t end = all.find_last_not_of("\r\n");
    if (end == std::string::npos) return false;
    size_t start = all.find_last_of('\n', end);
    std::string last = trim(all.substr(start == std::string::npos ? 0 : start + 1,
                                       end - (start == std::string::npos ? 0 : start + 1) + 1));
    headers = all.substr(0, start == std::string::npos ? 0 : start);

    size_t sp = last.find(' ');
    if (sp == std::string::npos) return false;
    httpCode = std::atoi(last.substr(0, sp).c_str());
    finalUrl = last.substr(sp + 1);
    return true;
}

static bool ask(const std::string& question, std::string& answer) {
    std::cerr << question << std::flush;
    bool ok = static_cast<bool>(std::getline(std::cin, answer));
    if (g_signal) {  // soru sırasında Ctrl+C: henüz geçici dosya yok, doğrudan çık
        std::cerr << "\nIptal edildi.\n";
        std::exit(128 + g_signal);
    }
    return ok;
}

static void usage(const char* prog) {
    std::cerr << "Kullanim: " << esc(prog) << " <url> [-o dosya_adi]\n"
              << "  -o   dosya adini elle ver (otomatik tespiti atlar)\n";
}

int main(int argc, char* argv[]) {
    std::string url, name;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
        if (a == "-o") {
            if (i + 1 >= argc) { usage(argv[0]); return 1; }
            std::string optVal = argv[++i];
            if (!optVal.empty() && optVal[0] == '-') {
                std::cerr << "Gecersiz dosya adi (secenek gibi baslayamaz): " << esc(optVal) << "\n";
                return 1;
            }
            if (optVal.find('/') != std::string::npos) {
                std::cerr << "Gecersiz dosya adi (dizin icermemeli, sadece ad verin): " << esc(optVal) << "\n";
                return 1;
            }
            name = sanitize(optVal, false);
            if (name.empty()) { std::cerr << "Gecersiz dosya adi: " << esc(optVal) << "\n"; return 1; }
        } else if (url.empty()) {
            url = a;
        } else {
            usage(argv[0]);
            return 1;
        }
    }
    if (url.empty()) { usage(argv[0]); return 1; }

    if (!validateAndNormalizeUrl(url)) {
        std::cerr << "Hata: Gecersiz URL (yalnizca http:// ve https:// desteklenir): " << esc(url) << "\n";
        return 1;
    }

    installSignalHandlers();
    bool interactive = isatty(STDIN_FILENO);

    if (name.empty()) {
        name = fromUrl(url);
        if (!hasExtension(name)) {
            std::string headers, finalUrl, candidate = name;
            int httpCode = 0;
            name.clear();
            if (probe(url, headers, finalUrl, httpCode)) {
                if (httpCode >= 200 && httpCode < 300)
                    name = fromContentDisposition(finalContentDisposition(headers));
                if (name.empty()) {
                    std::string fromFinal = fromUrl(finalUrl);
                    if (hasExtension(fromFinal)) name = fromFinal;
                    else if (candidate.empty()) candidate = fromFinal;
                }
            }
            if (g_signal) { std::cerr << "\nIptal edildi.\n"; return 128 + g_signal; }
            if (name.empty()) {
                if (!interactive) {
                    std::cerr << "Dosya adi bulunamadi, -o ile belirtin.\n";
                    return 2;
                }
                std::string answer;
                std::string q = "Dosya adi bulunamadi. Dosya adi" +
                                (candidate.empty() ? std::string() : " [" + esc(candidate) + "]") + ": ";
                if (!ask(q, answer)) return 2;
                name = sanitize(trim(answer), false);
                if (name.empty()) name = candidate;
                if (name.empty()) { std::cerr << "Dosya adi verilmedi.\n"; return 2; }
            }
        }
    }

    // Hedef kontrolü: dizin, symlink ve özel dosyalar reddedilir; normal dosya için onay istenir.
    struct stat lst;
    if (lstat(name.c_str(), &lst) == 0) {
        if (S_ISDIR(lst.st_mode)) {
            std::cerr << "Hata: Hedef bir dizindir: '" << esc(name) << "'\n";
            return 1;
        }
        if (S_ISLNK(lst.st_mode)) {
            std::cerr << "Hata: Hedef bir sembolik bagdir: '" << esc(name) << "'\n";
            return 1;
        }
        if (!S_ISREG(lst.st_mode)) {
            std::cerr << "Hata: Hedef normal bir dosya degil: '" << esc(name) << "'\n";
            return 1;
        }
        std::string answer;
        if (!interactive || !ask("'" + esc(name) + "' var. Uzerine yazilsin mi? [e/H]: ", answer) ||
            (answer != "e" && answer != "E" && answer != "y" && answer != "Y")) {
            std::cerr << "Iptal edildi.\n";
            return 3;
        }
    }

    // Geçici dosyaya indir (O_EXCL, symlink takip etmez), başarıda rename ile yerine koy.
    // Böylece yarım indirme hedefi bozmaz ve araya sokulan symlink üzerinden yazılmaz.
    std::string tmpl = ".curldl-XXXXXX";
    std::vector<char> tmpBuf(tmpl.begin(), tmpl.end());
    tmpBuf.push_back('\0');
    int fd = mkstemp(tmpBuf.data());
    if (fd < 0) { perror("gecici dosya olusturulamadi"); return 1; }
    std::string tmpPath(tmpBuf.data());
    mode_t mask = umask(0);
    umask(mask);
    fchmod(fd, 0666 & ~mask);

    std::cerr << "Indiriliyor: " << esc(name) << "\n";
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(fd);
        unlink(tmpPath.c_str());
        return 1;
    }
    if (pid == 0) {
        resetSignalHandlersInChild();
        if (dup2(fd, STDOUT_FILENO) < 0) _exit(127);
        close(fd);
        execCurl({"curl", "-q", "-L", "--fail", "--progress-bar",
                  "--proto", "=http,https", "--proto-redir", "=http,https",
                  "--max-redirs", "10",
                  "--url", url});
        perror("curl calistirilamadi (kurulu mu?)");
        _exit(127);
    }

    int status = 0;
    bool waited = waitChild(pid, status);

    // Son aşamada sinyalleri blokla: kontrol ile rename arasında gelen sinyal kaybolmasın.
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    sigaddset(&block, SIGHUP);
    sigprocmask(SIG_BLOCK, &block, &old);

    bool ok = waited && WIFEXITED(status) && WEXITSTATUS(status) == 0 && !g_signal;
    if (ok && fsync(fd) != 0) { perror("fsync"); ok = false; }
    close(fd);

    if (!ok) {
        unlink(tmpPath.c_str());
        if (g_signal) { std::cerr << "\nIptal edildi.\n"; return 128 + g_signal; }
        if (!waited) { perror("waitpid"); return 1; }
        if (WIFEXITED(status)) return WEXITSTATUS(status) ? WEXITSTATUS(status) : 1;
        return 1;
    }

    // rename öncesi son kontrol: hedef bu arada dizin olduysa üzerine taşıma
    if (lstat(name.c_str(), &lst) == 0 && S_ISDIR(lst.st_mode)) {
        std::cerr << "Hata: Hedef bir dizin haline geldi: '" << esc(name) << "'\n";
        unlink(tmpPath.c_str());
        return 1;
    }
    if (rename(tmpPath.c_str(), name.c_str()) != 0) {
        perror("rename");
        unlink(tmpPath.c_str());
        return 1;
    }
    return 0;
}
