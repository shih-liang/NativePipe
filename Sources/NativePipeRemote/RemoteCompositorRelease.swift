import NativePipeStrings
import Foundation

/// Release discovery runs asynchronously on the Mac. Select the installer and
/// compositor from one tag; VM-only and legacy releases are not installable.
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
                // Unauthenticated API calls are limited per network address,
                // and "HTTP 403" gives no hint that waiting is the whole fix.
                if status == 403 || status == 429 {
                    throw RemoteError.message(NPText("GitHub is limiting requests from your network. Try again in a few minutes."))
                }
                throw RemoteError.message(NPText("Couldn’t check GitHub for NativePipe compositor updates (HTTP %@). Check your internet connection, then try again.", String(status)))
            }
            let releases = try JSONDecoder().decode([Release].self, from: data)
            if let base = try select(releases) { return base }
            if releases.count < 100 { break }
        }
        throw RemoteError.message(NPText("GitHub has no NativePipe compositor release that can be installed. Try again later."))
    }

    static func select(_ releases: [Release]) throws -> URL? {
        let names = ["aarch64", "x86_64"].flatMap { arch in
            ["gnu", "musl"].map { "nativepipe-compositor-\(arch)-\($0).tar.gz" }
        }
        for release in releases where !release.draft && !release.prerelease {
            guard release.assets.contains(where: { $0.name == "SHA256SUMS" }),
                  release.assets.contains(where: { $0.name == "install-compositor.sh" }),
                  let asset = release.assets.first(where: { names.contains($0.name) }) else { continue }
            let url = asset.browser_download_url
            guard url.scheme == "https", url.host == "github.com", url.user == nil, url.password == nil,
                  url.query == nil, url.fragment == nil,
                  url.path.lowercased().hasPrefix("/shih-liang/nativepipe/releases/download/"),
                  url.lastPathComponent == asset.name else {
                throw RemoteError.message(NPText("GitHub returned an invalid NativePipe compositor download URL."))
            }
            return url.deletingLastPathComponent()
        }
        return nil
    }

}
