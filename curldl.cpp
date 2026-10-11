// curldl - curl ile indirmeyi kısaltır, dosya adını URL'den kendisi bulur.
// Kullanım: curldl <url> [-o dosya_adi]
// Tek dosyalı, bağımlılıksız saf C++ scripti (sistem curl komutunu çağırır)
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

// --- Dosya Adı Çıkarma ve Temizleme Fonksiyonları ---

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

// Windows/FAT uyumluluğu ve ayrılmış aygıt adlarını tespit eder (CON, PRN, AUX, NUL, COM1-9, LPT1-9)
static bool isReservedDeviceName(const std::string& name) {
    if (name.empty()) return false;
    std::string stem = name.substr(0, name.find('.'));
    for (char& c : stem) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL") return true;
    if (stem.size() == 4) {
        if ((stem.rfind("COM", 0) == 0 || stem.rfind("LPT", 0) == 0) &&
            stem[3] >= '1' && stem[3] <= '9') {
            return true;
        }
    }
    return false;
}

// Yol bileşenlerini atar (../../etc/passwd -> passwd), kontrol karakterlerini ve tehlikeli karakterleri siler
static std::string sanitize(const std::string& name) {
    std::string base = name;
    size_t slash = base.find_last_of("/\\");
    if (slash != std::string::npos) base = base.substr(slash + 1);

    // ':' ve tehlikeli karakterleri (NTFS stream, glob, control) reddet
    for (char c : base) {
        if (c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || c == '\0') {
            return "";
        }
    }

    std::string out;
    for (char c : base) {
        if (static_cast<unsigned char>(c) >= 32 && c != 127) out += c;
    }

    out = trim(out);

    // CLI bayrak/opsiyon enjeksiyonunu engellemek için baştaki tireleri temizle (-o, --config)
    while (!out.empty() && out.front() == '-') {
        out.erase(0, 1);
    }
    out = trim(out);

    if (out == "." || out == ".." || out.empty()) return "";
    if (isReservedDeviceName(out)) return "";
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

    return sanitize(urlDecode(path.substr(path.rfind('/') + 1)));
}

// Content-Disposition başlığından ad çıkarır (RFC 5987 / RFC 6266 uyumlu)
static std::string fromContentDisposition(const std::string& header) {
    std::string lower = header;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    // 1. Önce filename*= alanını ara (RFC 5987 / 6266)
    size_t fnStarPos = std::string::npos;
    size_t cur = 0;
    while (true) {
        size_t found = lower.find("filename*=", cur);
        if (found == std::string::npos) break;
        if (found == 0 || lower[found - 1] == ';' || std::isspace(static_cast<unsigned char>(lower[found - 1]))) {
            fnStarPos = found;
            break;
        }
        cur = found + 10;
    }

    if (fnStarPos != std::string::npos) {
        std::string v = header.substr(fnStarPos + 10);
        v = trim(v.substr(0, v.find(';')));
        if (!v.empty() && v.front() == '"' && v.back() == '"' && v.size() >= 2) {
            v = v.substr(1, v.size() - 2);
        }
        size_t q1 = v.find('\'');
        if (q1 != std::string::npos) {
            size_t q2 = v.find('\'', q1 + 1);
            if (q2 != std::string::npos) {
                std::string encoded = v.substr(q2 + 1);
                std::string decoded = urlDecode(encoded);
                std::string candidate = sanitize(decoded);
                if (!candidate.empty()) return candidate;
            }
        }
        // filename*= geçersizse veya temizlenince boş kalırsa filename= alanına geri dön
    }

    // 2. filename= alanını ara
    size_t fnPos = std::string::npos;
    cur = 0;
    while (true) {
        size_t found = lower.find("filename=", cur);
        if (found == std::string::npos) break;
        if (found == 0 || lower[found - 1] == ';' || std::isspace(static_cast<unsigned char>(lower[found - 1]))) {
            fnPos = found;
            break;
        }
        cur = found + 9;
    }

    if (fnPos != std::string::npos) {
        std::string v = trim(header.substr(fnPos + 9));
        if (!v.empty() && v[0] == '"') {
            std::string unescaped;
            bool escape = false;
            for (size_t i = 1; i < v.size(); ++i) {
                if (escape) {
                    unescaped += v[i];
                    escape = false;
                } else if (v[i] == '\\') {
                    escape = true;
                } else if (v[i] == '"') {
                    break;
                } else {
                    unescaped += v[i];
                }
            }
            std::string candidate = sanitize(unescaped);
            if (!candidate.empty()) return candidate;
        } else {
            std::string raw = trim(v.substr(0, v.find(';')));
            std::string candidate = sanitize(raw);
            if (!candidate.empty()) return candidate;
        }
    }
    return "";
}

// URL şemasını doğrular ve normalize eder (yalnızca http ve https izinli)
static bool validateAndNormalizeUrl(std::string& url) {
    if (url.empty()) return false;

    size_t colon = url.find(':');
    size_t slash = url.find('/');

    if (colon != std::string::npos && (slash == std::string::npos || colon < slash)) {
        std::string scheme = url.substr(0, colon);
        for (char& c : scheme) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        if (url.size() > colon + 2 && url[colon + 1] == '/' && url[colon + 2] == '/') {
            if (scheme != "http" && scheme != "https") {
                return false;
            }
            return true;
        }

        // Host:port kontrolü (örn. localhost:8080/dosya)
        bool isPort = true;
        for (size_t i = colon + 1; i < (slash == std::string::npos ? url.size() : slash); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(url[i]))) {
                isPort = false;
                break;
            }
        }
        if (isPort && colon + 1 < (slash == std::string::npos ? url.size() : slash)) {
            url = "https://" + url;
            return true;
        }

        return false;
    }

    url = "https://" + url;
    return true;
}

// --- Ana Program ---

static bool probe(const std::string& url, std::string& headers, std::string& finalUrl) {
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }

    if (pid == 0) {
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) {
            _exit(127);
        }
        close(pipefd[1]);

        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        const char* args[] = {
            "curl",
            "-sIL",
            "--proto", "=http,https",
            "--proto-redir", "=http,https",
            "--max-time", "15",
            "-w", "\n%{url_effective}\n",
            "--url", url.c_str(),
            nullptr
        };

        execvp("curl", const_cast<char* const*>(args));
        _exit(127);
    }

    close(pipefd[1]);
    std::string all;
    char buf[4096];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0 || (n < 0 && errno == EINTR)) {
        if (n > 0) {
            all.append(buf, static_cast<size_t>(n));
        }
    }
    close(pipefd[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return false;
        }
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return false;
    }

    size_t end = all.find_last_not_of("\r\n");
    if (end == std::string::npos) return false;
    size_t start = all.find_last_of('\n', end);
    finalUrl = trim(all.substr(start == std::string::npos ? 0 : start + 1, end - start));
    headers = all.substr(0, start == std::string::npos ? 0 : start);
    return true;
}

static std::string lastContentDisposition(const std::string& headers) {
    std::string found;
    size_t pos = 0;
    while (pos < headers.size()) {
        size_t eol = headers.find('\n', pos);
        if (eol == std::string::npos) eol = headers.size();
        std::string line = headers.substr(pos, eol - pos);
        if (line.size() > 20 && trim(line).size() > 20) {
            std::string head = line.substr(0, 20);
            for (char& c : head) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (head == "content-disposition:") found = line;
        }
        pos = eol + 1;
    }
    return found;
}

static bool ask(const std::string& question, std::string& answer) {
    std::cerr << question << std::flush;
    return static_cast<bool>(std::getline(std::cin, answer));
}

static void usage(const char* prog) {
    std::cerr << "Kullanim: " << prog << " <url> [-o dosya_adi]\n"
              << "  -o   dosya adini elle ver (otomatik tespiti atlar)\n";
}

#ifndef CURLDL_NO_MAIN
int main(int argc, char* argv[]) {
    std::string url, name;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
        if (a == "-o") {
            if (i + 1 >= argc) { usage(argv[0]); return 1; }
            std::string optVal = argv[++i];
            if (!optVal.empty() && optVal[0] == '-') {
                std::cerr << "Gecersiz dosya adi (secenek gibi baslayamaz): " << optVal << "\n";
                return 1;
            }
            name = sanitize(optVal);
            if (name.empty()) { std::cerr << "Gecersiz dosya adi.\n"; return 1; }
        } else if (url.empty()) {
            url = a;
        } else {
            usage(argv[0]);
            return 1;
        }
    }
    if (url.empty()) { usage(argv[0]); return 1; }

    if (!validateAndNormalizeUrl(url)) {
        std::cerr << "Hata: Gecersiz veya desteklenmeyen URL semasi (yalnizca http:// ve https:// desteklenir): " << url << "\n";
        return 1;
    }

    bool interactive = isatty(STDIN_FILENO);

    if (name.empty()) {
        name = fromUrl(url);
        if (!hasExtension(name)) {
            std::string headers, finalUrl, candidate = name;
            name.clear();
            if (probe(url, headers, finalUrl)) {
                name = fromContentDisposition(lastContentDisposition(headers));
                if (name.empty()) {
                    std::string fromFinal = fromUrl(finalUrl);
                    if (hasExtension(fromFinal)) name = fromFinal;
                    else if (candidate.empty()) candidate = fromFinal;
                }
            }
            if (name.empty()) {
                if (!interactive) {
                    std::cerr << "Dosya adi bulunamadi, -o ile belirtin.\n";
                    return 2;
                }
                std::string answer;
                std::string q = "Dosya adi bulunamadi. Dosya adi" +
                                (candidate.empty() ? std::string() : " [" + candidate + "]") + ": ";
                if (!ask(q, answer)) return 2;
                name = sanitize(trim(answer));
                if (name.empty()) name = candidate;
                if (name.empty()) { std::cerr << "Dosya adi verilmedi.\n"; return 2; }
            }
        }
    }

    // SEC-04 & SEC-10: Dizin ve sembolik bağ (symlink) güvenlik kontrolleri
    struct stat lst;
    if (lstat(name.c_str(), &lst) == 0) {
        if (S_ISDIR(lst.st_mode)) {
            std::cerr << "Hata: Hedef bir dizindir: '" << name << "'\n";
            return 1;
        }
        if (S_ISLNK(lst.st_mode)) {
            std::cerr << "Hata: Hedef bir sembolik bagdir (symlink overwrite engellendi): '" << name << "'\n";
            return 1;
        }
        std::string answer;
        if (!interactive || !ask("'" + name + "' var. Uzerine yazilsin mi? [e/H]: ", answer) ||
            (answer != "e" && answer != "E" && answer != "y" && answer != "Y")) {
            std::cerr << "Iptal edildi.\n";
            return 3;
        }
    } else {
        struct stat st;
        if (stat(name.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            std::cerr << "Hata: Hedef bir dizindir: '" << name << "'\n";
            return 1;
        }
    }

    std::cerr << "Indiriliyor: " << name << "\n";
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }
    if (pid == 0) {
        execlp("curl", "curl", "-L", "--fail", "--progress-bar",
               "--proto", "=http,https", "--proto-redir", "=http,https",
               "-o", name.c_str(),
               "--url", url.c_str(), static_cast<char*>(nullptr));
        perror("curl calistirilamadi (kurulu mu?)");
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            perror("waitpid");
            return 1;
        }
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
#endif
