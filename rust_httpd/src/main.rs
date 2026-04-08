use std::io::{Read, Write};
use std::net::TcpListener;
use std::sync::{Arc, Mutex};
use std::thread;
fn main() {
    let listener = TcpListener::bind("127.0.0.1:4000").unwrap();
    println!("🚀 Rust 版 Tinyhttpd 已启动，监听端口: 4000");
// 【新增】创建一个安全的并发计数器。Arc 负责跨线程共享，Mutex 负责加锁保护
    let request_count = Arc::new(Mutex::new(0));

    for stream in listener.incoming() {
        let mut stream = stream.unwrap();
// 【多线程进化】在把计数器扔进新线程之前，先给它配一把“备用钥匙”
        let counter_clone = Arc::clone(&request_count);

        // 【多线程进化】使用 spawn 开启新线程，使用 move 把流和钥匙的所有权转移进去
        thread::spawn(move || {
        let mut buffer = [0; 1024];

        stream.read(&mut buffer).unwrap();
        let request = String::from_utf8_lossy(&buffer[..]);
let request_line = request.lines().next().unwrap_or("");
// 拿到当前的线程 ID，方便我们在终端里看是不是真的多线程了
            let thread_id = thread::current().id();
            println!("【线程 {:?} 收到请求】: {}", thread_id, request_line);
        //println!("【收到请求】: {}", request.lines().next().unwrap_or(""));

       // 注意：这里不需要手动写 unlock！当 count 变量离开作用域时，Rust 会自动解锁。
        let current_count = {
            let mut count = counter_clone.lock().unwrap();
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

// 【新增】云端极客笔记本 Pro 版路由
        } else if method == "GET" && path == "/notebook" {
            let html = format!(r#"
            <!DOCTYPE html>
            <html>
            <head>
                <meta charset="utf-8">
                <title>NPU 云端极客笔记本 - Pro版</title>
                
                //<link rel="stylesheet" href="https://cdn.jsdelivr.net/simplemde/latest/simplemde.min.css">
                //<script src="https://cdn.jsdelivr.net/simplemde/latest/simplemde.min.js"></script>
                //<link rel="stylesheet" href="https://cdn.bootcdn.net/ajax/libs/highlight.js/11.7.0/styles/github.min.css">
                //<script src="https://cdn.bootcdn.net/ajax/libs/highlight.js/11.7.0/highlight.min.js"></script>
                //<link rel="stylesheet" href="https://cdn.bootcdn.net/ajax/libs/font-awesome/4.7.0/css/font-awesome.min.css">
<link rel="stylesheet" href="https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/styles/vs2015.min.css">
    <script src="https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/highlight.min.js"></script>
    
    <link rel="stylesheet" href="https://cdn.jsdelivr.net/simplemde/latest/simplemde.min.css">
    <script src="https://cdn.jsdelivr.net/simplemde/latest/simplemde.min.js"></script>
    
    <link rel="stylesheet" href="https://cdn.bootcdn.net/ajax/libs/font-awesome/4.7.0/css/font-awesome.min.css">
                <style>
                    body {{ font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background: #f4f7f6; margin: 0; padding: 0; }}
                    .header {{ background: #2c3e50; color: white; padding: 15px 20px; text-align: center; box-shadow: 0 4px 6px rgba(0,0,0,0.1); }}
                    .container {{ max-width: 1200px; margin: 20px auto; background: white; padding: 20px; border-radius: 8px; box-shadow: 0 8px 16px rgba(0,0,0,0.1); height: 80vh; display: flex; flex-direction: column; }}
                    .visitor-badge {{ background: #e74c3c; color: white; padding: 4px 10px; border-radius: 12px; font-size: 14px; margin-left: 10px; vertical-align: middle; }}
                    .btn-download {{ display: block; width: 100%; background: #27ae60; color: white; border: none; padding: 15px; font-size: 18px; border-radius: 6px; font-weight: bold; cursor: pointer; margin-top: 15px; transition: 0.3s; }}
                    .btn-download:hover {{ background: #2ecc71; }}
                    .CodeMirror {{ flex-grow: 1; font-size: 16px; }} 
                </style>
            </head>
            <body>
                <div class="header">
                    <h2>📝 NPU 云端极客笔记 (Pro 版) <span class="visitor-badge">并发处理请求: {} 次</span></h2>
                </div>
                <div class="container">
                    <textarea id="my_editor"></textarea>
                    <button class="btn-download" onclick="downloadNote()"><i class="fa fa-download"></i> 💾 封存笔记并下载到本机 (.md)</button>
                </div>
                
                <script>
                    var simplemde = new SimpleMDE({{
                        element: document.getElementById("my_editor"),
                        spellChecker: false,
                        autofocus: true,
                        // 开启本地自动保存黑科技
                        autosave: {{
                            enabled: true,
                            uniqueId: "NpuGeekNote",
                            delay: 1000,
                        }},
                        // 开启代码语法高亮
                        renderingConfig: {{
                            codeSyntaxHighlighting: true
                        }}
                    }});
                    
                    // 强制开启左右分屏模式
      //              simplemde.toggleSideBySide();
                    
                    // 触发本地下载的魔法函数
                    function downloadNote() {{
                        var text = simplemde.value();
                        if (!text) {{ alert("哎呀，你连字都还没敲呢！"); return; }}
                        
                        var blob = new Blob([text], {{ type: "text/markdown;charset=utf-8" }});
                        var link = document.createElement("a");
                        link.href = URL.createObjectURL(blob);
                        link.download = "NPU_极客笔记_机密文件.md";
                        
                        document.body.appendChild(link);
                        link.click();
                        document.body.removeChild(link);
                    }}
                </script>
            </body>
            </html>
            "#, current_count);
            
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
    });
  }
}
