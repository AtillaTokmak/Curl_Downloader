// curldl - curl ile indirmeyi kısaltır, dosya adını URL'den kendisi bulur.
// Kullanım: curldl <url> [-o dosya_adi]
// Tek dosyalı, bağımlılıksız saf C++ scripti (sistem curl komutunu çağırır)
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cctype>
#include <cstdio>
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

// Yol bileşenlerini atar (../../etc/passwd -> passwd), kontrol karakterlerini siler
static std::string sanitize(const std::string& name) {
    std::string base = name;
    size_t slash = base.find_last_of("/\\");
    if (slash != std::string::npos) base = base.substr(slash + 1);

    std::string out;
    for (char c : base)
        if (static_cast<unsigned char>(c) >= 32 && c != 127) out += c;

    out = trim(out);
    if (out == "." || out == "..") return "";
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

// Content-Disposition başlığından ad çıkarır
static std::string fromContentDisposition(const std::string& header) {
    std::string lower = header;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    size_t pos = lower.find("filename*=");
    if (pos != std::string::npos) {
        std::string v = header.substr(pos + 10);
        v = trim(v.substr(0, v.find(';')));
        size_t q = v.find("''");
        if (q != std::string::npos) v = v.substr(q + 2);
        return sanitize(urlDecode(v));
    }

    pos = lower.find("filename=");
    if (pos == std::string::npos) return "";
    std::string v = trim(header.substr(pos + 9));
    if (!v.empty() && v[0] == '"') {
        v = v.substr(1);
        v = v.substr(0, v.find('"'));
    } else {
        v = trim(v.substr(0, v.find(';')));
    }
    return sanitize(v);
}

// --- Ana Program ---

static std::string shellQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out + "'";
}

static bool probe(const std::string& url, std::string& headers, std::string& finalUrl) {
    std::string cmd = "curl -sIL --max-time 15 -w '\\n%{url_effective}\\n' " +
                      shellQuote(url) + " 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return false;
    std::string all;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) all.append(buf, n);
    pclose(p);

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

static bool fileExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

static void usage(const char* prog) {
    std::cerr << "Kullanim: " << prog << " <url> [-o dosya_adi]\n"
              << "  -o   dosya adini elle ver (otomatik tespiti atlar)\n";
}

int main(int argc, char* argv[]) {
    std::string url, name;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
        if (a == "-o") {
            if (i + 1 >= argc) { usage(argv[0]); return 1; }
            name = sanitize(argv[++i]);
            if (name.empty()) { std::cerr << "Gecersiz dosya adi.\n"; return 1; }
        } else if (url.empty()) {
            url = a;
        } else {
            usage(argv[0]);
            return 1;
        }
    }
    if (url.empty()) { usage(argv[0]); return 1; }
    if (url.find("://") == std::string::npos) url = "https://" + url;

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

    if (fileExists(name)) {
        std::string answer;
        if (!interactive || !ask("'" + name + "' var. Uzerine yazilsin mi? [e/H]: ", answer) ||
            (answer != "e" && answer != "E" && answer != "y" && answer != "Y")) {
            std::cerr << "Iptal edildi.\n";
            return 3;
        }
    }

    std::cerr << "Indiriliyor: " << name << "\n";
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }
    if (pid == 0) {
        execlp("curl", "curl", "-L", "--fail", "--progress-bar", "-o", name.c_str(),
               "--url", url.c_str(), static_cast<char*>(nullptr));
        perror("curl calistirilamadi (kurulu mu?)");
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
