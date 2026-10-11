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

## Neden C++?

Tek bir yerel binary üretir, .NET runtime gerekmez. Program sistemdeki `curl`'ü çağırır, `libcurl-dev` de gerekmez. Sadece `g++` ve `curl` yeter.

## Derleme

```
make
sudo make install
```
