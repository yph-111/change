use std::io::{Read, Write};
use std::net::TcpListener;
fn main() {
    let listener = TcpListener::bind("127.0.0.1:4000").unwrap();
    println!("🚀 Rust 版 Tinyhttpd 已启动，监听端口: 4000");

    for stream in listener.incoming() {
        let mut stream = stream.unwrap();
        let mut buffer = [0; 1024];

        stream.read(&mut buffer).unwrap();
        let request = String::from_utf8_lossy(&buffer[..]);
        println!("【收到请求】: {}", request.lines().next().unwrap_or(""));

        // 利用 Rust 的原生字符串 (r#"..."#) 轻松编写完整的 HTML 和 CSS
        let html_content = r#"
        <!DOCTYPE html>
        <html>
        <head>
            <meta charset="utf-8">
            <title>Rust Server Status</title>
            <style>
                body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background-color: #f0f2f5; margin: 0; padding: 50px; display: flex; justify-content: center; }
                .card { background: #ffffff; padding: 40px; border-radius: 12px; box-shadow: 0 8px 16px rgba(0,0,0,0.1); width: 100%; max-width: 600px; }
                h1 { color: #DD5144; margin-top: 0; }
                ul { list-style-type: none; padding: 0; font-size: 18px; line-height: 2; }
                li { border-bottom: 1px solid #eee; padding-bottom: 10px; margin-bottom: 10px; }
                .highlight { color: #0078D7; font-weight: bold; }
            </style>
        </head>
        <body>
            <div class="card">
                <h1>🦀 Rust 重构版监控面板</h1>
                <ul>
                    <li><strong>系统状态：</strong> 🟢 运行中（内存绝对安全）</li>
                    <li><strong>底层架构：</strong> <span class="highlight">Rust std::net 零成本抽象</span></li>
                    <li><strong>重构进度：</strong> HTTP 响应渲染测试通过！</li>
                </ul>
            </div>
        </body>
        </html>
        "#;

        // 拼接 HTTP 响应头，注意 Content-Type 改成了 text/html
        let response = format!(
            "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: {}\r\n\r\n{}",
            html_content.len(), // Rust 直接计算字节长度，极其方便
            html_content
        );

        stream.write_all(response.as_bytes()).unwrap();
        stream.flush().unwrap();
    }
}
