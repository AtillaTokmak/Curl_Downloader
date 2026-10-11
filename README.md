# curldl

`curl -L --fail --progress-bar -o dosya.zip "https://..."` yerine:

```
curldl https://example.com/dosya.zip
curldl "https://example.com/indir?id=5"
curldl https://example.com/indir -o ad.bin
```

Dosya adı sırayla şuradan bulunur:

1. `-o` ile verilen ad
2. URL yolundaki son parça (uzantısı varsa; `?sorgu`, `#fragment` atılır, `%20` çözülür)
3. Sunucunun `Content-Disposition` başlığı veya yönlendirme sonrası URL
4. Hiçbiri olmazsa kullanıcıya sorulur (terminal değilse hata verip çıkar)

Aynı adlı dosya varsa üzerine yazmadan önce sorar.

## Güvenlik davranışı

- Sadece `http://` ve `https://` kabul edilir; yönlendirmeler de bu protokollerle sınırlıdır (en fazla 10).
- `~/.curlrc` yok sayılır (`curl -q`).
- İndirme önce gizli bir geçici dosyaya (`.curldl-XXXXXX`) yapılır, başarılı olursa hedefe taşınır. Hata veya Ctrl+C durumunda mevcut dosya bozulmaz, yarım dosya kalmaz.
- Hedef bir dizin, sembolik bağ veya özel dosyaysa indirme yapılmaz.
- Sunucudan/URL'den gelen adlarda baştaki `.` ve `-` silinir (`.bashrc` -> `bashrc`); `-o` ile dizin içeren ad verilemez.
- Terminale basılan ad/URL'lerdeki kontrol karakterleri `\xNN` olarak gösterilir.

## Neden C++?

Tek bir yerel binary üretir, .NET runtime gerekmez. Program sistemdeki `curl`'ü çağırır, `libcurl-dev` de gerekmez. Sadece `g++` ve `curl` yeter.

## Derleme

```
make
sudo make install
```
