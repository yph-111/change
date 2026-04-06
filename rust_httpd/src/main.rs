use std::io::{Read, Write};
use std::net::TcpListener;
use std::sync::{Arc, Mutex};
fn main() {
    let listener = TcpListener::bind("127.0.0.1:4000").unwrap();
    println!("🚀 Rust 版 Tinyhttpd 已启动，监听端口: 4000");
// 【新增】创建一个安全的并发计数器。Arc 负责跨线程共享，Mutex 负责加锁保护
    let request_count = Arc::new(Mutex::new(0));

    for stream in listener.incoming() {
        let mut stream = stream.unwrap();
        let mut buffer = [0; 1024];

        stream.read(&mut buffer).unwrap();
        let request = String::from_utf8_lossy(&buffer[..]);
let request_line = request.lines().next().unwrap_or("");
        println!("【收到请求】: {}", request.lines().next().unwrap_or(""));

       // 注意：这里不需要手动写 unlock！当 count 变量离开作用域时，Rust 会自动解锁。
        let current_count = {
            let mut count = request_count.lock().unwrap();
            *count += 1;
            *count // 返回当前最新的值
        };

        let mut parts = request_line.split_whitespace();
        let method = parts.next().unwrap_or("");
        let path = parts.next().unwrap_or("");

        let (status_code, content_type, content) = if method == "GET" && path == "/status" {
            // 注意看：CSS 里的括号全变成了双层的 {{ }}
            let html = format!(r#"
        <!DOCTYPE html>
        <html>
        <head>
            <meta charset="utf-8">
            <title>Rust Server Status</title>
            <style>
                body {{ font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background-color: #f0f2f5; margin: 0; padding: 50px; display: flex; justify-content: center; }}
                .card {{ background: #ffffff; padding: 40px; border-radius: 12px; box-shadow: 0 8px 16px rgba(0,0,0,0.1); width: 100%; max-width: 600px; }}
                h1 {{ color: #DD5144; margin-top: 0; }}
                ul {{ list-style-type: none; padding: 0; font-size: 18px; line-height: 2; }}
                li {{ border-bottom: 1px solid #eee; padding-bottom: 10px; margin-bottom: 10px; }}
                .highlight {{ color: #0078D7; font-weight: bold; }}
            </style>
        </head>
        <body>
            <div class="card">
                <h1>🦀 Rust 重构版监控面板</h1>
                <ul>
                    <li><strong>系统状态：</strong> 🟢 运行中（内存绝对安全）</li>
                    <li><strong>底层架构：</strong> <span class="highlight">Rust std::net 零成本抽象</span></li>
                    
                    <li><strong>累计处理请求数：</strong> <span class="highlight">{}</span> 次</li>
                </ul>
            </div>
        </body>
        </html>
        "#, current_count); // current_count 就会精准落入上面那个 {} 里

            ("200 OK", "text/html", html)

        } else if method == "GET" && path == "/" {
            ("200 OK", "text/plain", format!("欢迎来到根目录！你是第 {} 个访客。请访问 /status 查看面板。", current_count))
        } else {
            ("404 NOT FOUND", "text/plain", "404 错误：页面被外星人抓走啦！".to_string())
        };
        // 拼接 HTTP 响应头，注意 Content-Type 改成了 text/html
        let response = format!(
           "HTTP/1.1 {}\r\nContent-Type: {}; charset=utf-8\r\nContent-Length: {}\r\n\r\n{}",
            status_code, content_type, content.len(), content
        );

        stream.write_all(response.as_bytes()).unwrap();
        stream.flush().unwrap();
    }
}
