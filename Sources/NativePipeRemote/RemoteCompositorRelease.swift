import Foundation

/// Release discovery runs asynchronously on the Mac; Linux only needs curl,
/// sha256sum and tar. VM runtime releases must not hide compositor releases.
enum RemoteCompositorRelease {
    struct Release: Decodable {
        struct Asset: Decodable { let name: String; let browser_download_url: URL }
        let draft: Bool
        let prerelease: Bool
        let assets: [Asset]
    }

    static func latest() async throws -> URL {
        for page in 1...10 {
            let url = URL(string: "https://api.github.com/repos/shih-liang/NativePipe/releases?per_page=100&page=\(page)")!
            var request = URLRequest(url: url, cachePolicy: .reloadIgnoringLocalCacheData, timeoutInterval: 20)
            request.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
            let (data, response) = try await URLSession.shared.data(for: request)
            guard let response = response as? HTTPURLResponse, response.statusCode == 200 else {
                let status = (response as? HTTPURLResponse)?.statusCode ?? 0
                throw RemoteError.message("Could not check NativePipe updates on GitHub (HTTP \(status)).")
            }
            let releases = try JSONDecoder().decode([Release].self, from: data)
            if let base = try select(releases) { return base }
            if releases.count < 100 { break }
        }
        throw RemoteError.message("GitHub has no NativePipe compositor release available. The repository needs a release containing nativepipe-compositor-<architecture>-<libc>.tar.gz and SHA256SUMS. VM runtime packages cannot be used for remote connections.")
    }

    static func select(_ releases: [Release]) throws -> URL? {
        let names = ["aarch64", "x86_64"].flatMap { arch in
            ["gnu", "musl"].map { "nativepipe-compositor-\(arch)-\($0).tar.gz" }
        }
        for release in releases where !release.draft && !release.prerelease {
            guard release.assets.contains(where: { $0.name == "SHA256SUMS" }),
                  let asset = release.assets.first(where: { names.contains($0.name) }) else { continue }
            let url = asset.browser_download_url
            guard url.scheme == "https", url.host == "github.com", url.user == nil, url.password == nil,
                  url.query == nil, url.fragment == nil,
                  url.path.lowercased().hasPrefix("/shih-liang/nativepipe/releases/download/"),
                  url.lastPathComponent == asset.name else {
                throw RemoteError.message("GitHub returned an invalid NativePipe compositor download URL.")
            }
            return url.deletingLastPathComponent()
        }
        return nil
    }

}
