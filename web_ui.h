#ifndef ESP_WEB_UI_H
#define ESP_WEB_UI_H

// ========== HTML前端内容（含Web编辑器）==========
const char HTML_CONTENT[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP32-QINUX Terminal</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body { background: #000; display: flex; justify-content: center; align-items: center; min-height: 100vh; font-family: 'Courier New', monospace; overflow: hidden; transition: background 0.3s; }
        body.matrix-active { background: #000 !important; }
        .crt { width: 95vw; max-width: 1200px; aspect-ratio: 4 / 3; max-height: 92vh; background: #1a1a1a; border-radius: 20px; padding: clamp(10px, 2vw, 30px); box-shadow: 0 0 50px rgba(0, 255, 0, 0.15), inset 0 0 20px rgba(0, 0, 0, 0.8); position: relative; display: flex; flex-direction: column; }
        .screen { flex: 1; width: 100%; background: #000000; border: 3px solid #003300; border-radius: 10px; padding: clamp(5px, 1.5vw, 15px); color: #00ff00; font-size: clamp(9px, 1.4vw, 18px); line-height: 1.2; position: relative; overflow: hidden; text-shadow: 0 0 clamp(1px, 0.3vw, 3px) #00ff00; display: flex; flex-direction: column; }
        .scanline { position: absolute; top: 0; left: 0; width: 100%; height: 100%; background: linear-gradient(to bottom, transparent 50%, rgba(0, 30, 0, 0.25) 50%); background-size: 100% clamp(4px, 1vw, 8px); pointer-events: none; animation: scan 4s linear infinite; z-index: 1; }
        .screen::after { content: ''; position: absolute; top: 0; left: 0; width: 100%; height: 100%; background: rgba(0, 255, 0, 0.02); pointer-events: none; animation: flicker 0.15s infinite alternate; z-index: 1; }
        @keyframes scan { 0% { transform: translateY(0); } 100% { transform: translateY(-4px); } }
        @keyframes flicker { 0% { opacity: 0.97; } 100% { opacity: 1; } }
        #output { flex: 1; width: 100%; overflow-y: auto; white-space: pre-wrap; word-wrap: break-word; margin-bottom: clamp(5px, 1vw, 10px); z-index: 2; position: relative; }
        #output::-webkit-scrollbar { width: clamp(4px, 0.8vw, 8px); }
        #output::-webkit-scrollbar-thumb { background: #005500; border-radius: 3px; }
        .input-line { display: flex; align-items: center; width: 100%; flex-shrink: 0; z-index: 2; position: relative; }
        .prompt { color: #00ff00; margin-right: clamp(4px, 0.5vw, 8px); flex-shrink: 0; }
        #input { flex: 1; background: transparent; border: none; outline: none; color: #00ff00; font-family: 'Courier New', monospace; font-size: inherit; text-shadow: 0 0 clamp(1px, 0.3vw, 3px) #00ff00; }
        #hiddenFileInput { display: none; }
        #matrix-canvas { position: absolute; top: 0; left: 0; width: 100%; height: 100%; background: #000; z-index: 10; display: none; }
        .matrix-active #output, .matrix-active .input-line { display: none !important; }
        .cat-container { position: fixed; top: 0; left: 0; width: 100vw; height: 100vh; background: #000; overflow: hidden; z-index: 150; display: none; pointer-events: none; }
        .falling-cat { position: absolute; top: -10%; animation: fall linear forwards; will-change: transform, opacity; user-select: none; pointer-events: none; filter: drop-shadow(0 0 2px rgba(0,255,0,0.3)); }
        @keyframes fall { 0% { transform: translateY(0) rotate(-10deg); opacity: 0; } 5% { opacity: 0.9; } 95% { opacity: 0.9; } 100% { transform: translateY(115vh) rotate(20deg); opacity: 0; } }
        .cat-active .crt { display: none !important; }
        .cat-active body { background: #000 !important; }
        .fortune-text { color: #00ff88; font-style: italic; text-shadow: 0 0 5px #00ff88; animation: glow 2s ease-in-out infinite alternate; display: block; margin: 4px 0; }
        @keyframes glow { from { text-shadow: 0 0 3px #00ff88; } to { text-shadow: 0 0 15px #00ff88, 0 0 30px #00aa55; } }
        .exit-hint { position: fixed; bottom: 20px; left: 50%; transform: translateX(-50%); color: #0f0; background: rgba(0, 20, 0, 0.85); padding: 8px 16px; border-radius: 4px; z-index: 200; font-size: clamp(10px, 2vw, 14px); border: 1px solid #005500; animation: hintFade 3s forwards; }
        @keyframes hintFade { 0%, 80% { opacity: 1; } 100% { opacity: 0; } }
        /* 编辑器样式 */
        #editorContainer { display: none; flex: 1; flex-direction: column; z-index: 3; position: relative; }
        #editorToolbar { display: flex; justify-content: space-between; align-items: center; padding: 4px 0; }
        #editorFilename { color: #0f0; }
        #editorTextarea { flex: 1; width: 100%; background: #000; color: #0f0; border: 1px solid #0f0; font-family: 'Courier New', monospace; font-size: inherit; line-height: 1.2; text-shadow: 0 0 3px #00ff00; resize: none; padding: 5px; box-sizing: border-box; }
        #editorSaveBtn { background: #0a0; color: #000; border: none; padding: 2px 8px; cursor: pointer; }
        #editorCancelBtn { background: #a00; color: #fff; border: none; padding: 2px 8px; cursor: pointer; }
        #editorSaveBtn:hover { background: #0f0; }
        #editorCancelBtn:hover { background: #f00; }
        #editorCursorPos { color: #0a0; margin-right: 10px; font-size: 0.85em; }
        #editorHints { color: #080; margin-right: 10px; font-size: 0.8em; }
    </style>
</head>
<body>
    <input type="file" id="hiddenFileInput">
    <canvas id="matrix-canvas"></canvas>
    <div class="cat-container" id="catContainer"></div>
    <div class="crt">
        <div class="screen">
            <div class="scanline"></div>
            <div id="output"></div>
            <div class="input-line">
                <span class="prompt" id="prompt">root@esp32:/# </span>
                <input type="text" id="input" autocomplete="off" autofocus>
            </div>
            <div id="editorContainer">
                <div id="editorToolbar">
                    <span id="editorFilename"></span>
                    <div>
                        <span id="editorHints">Ctrl+S Save · Esc Cancel</span>
                        <span id="editorCursorPos">Ln 1, Col 1</span>
                        <button id="editorSaveBtn">Save</button>
                        <button id="editorCancelBtn">Cancel</button>
                    </div>
                </div>
                <textarea id="editorTextarea"></textarea>
            </div>
        </div>
    </div>
    <script>
        const output = document.getElementById('output');
        const input = document.getElementById('input');
        const prompt = document.getElementById('prompt');
        const fileInput = document.getElementById('hiddenFileInput');
        const catContainer = document.getElementById('catContainer');
        const matrixCanvas = document.getElementById('matrix-canvas');
        const matrixCtx = matrixCanvas.getContext('2d');
        const CHUNK_SIZE = 4096;
        let ws;
        let matrixInterval = null;
        let catSpawnInterval = null;
        const CAT_EMOJIS = ['🐱', '😺', '😸', '😻', '😽', '🙀', '😿', '😾', '🐈', '🐈‍⬛', '🐾'];
        const cmdHistory = [];
        let histPos = -1;
        let isEditingMode = false;
        let isEditorMode = false;
        let currentEditFilename = '';
        let lastDlProg = -1;
        let lastUlProg = -1;
        let uploadComplete = false;

        const editorContainer = document.getElementById('editorContainer');
        const editorTextarea = document.getElementById('editorTextarea');
        const editorFilename = document.getElementById('editorFilename');
        const editorSaveBtn = document.getElementById('editorSaveBtn');
        const editorCancelBtn = document.getElementById('editorCancelBtn');
        const editorCursorPos = document.getElementById('editorCursorPos');

        function enterEditorMode(filename, content) {
            document.querySelector('.input-line').style.display = 'none';
            editorContainer.style.display = 'flex';
            editorFilename.textContent = filename;
            editorTextarea.value = content;
            currentEditFilename = filename;
            isEditorMode = true;
            editorTextarea.focus();
        }

        function exitEditorMode() {
            document.querySelector('.input-line').style.display = 'flex';
            editorContainer.style.display = 'none';
            isEditorMode = false;
            currentEditFilename = '';
            input.focus();
        }

        editorSaveBtn.addEventListener('click', () => {
            if (!isEditorMode) return;
            const content = editorTextarea.value;
            const encoder = new TextEncoder();
            const utf8Bytes = encoder.encode(content);
            let binary = '';
            for (let i = 0; i < utf8Bytes.length; i++)
                binary += String.fromCharCode(utf8Bytes[i]);
            const b64 = btoa(binary);
            ws.send('__EDIT_SAVE__:' + currentEditFilename + ':' + b64);
            exitEditorMode();
        });

        editorCancelBtn.addEventListener('click', () => { exitEditorMode(); });

        editorTextarea.addEventListener('keydown', (e) => {
            if ((e.ctrlKey || e.metaKey) && e.key === 's') { e.preventDefault(); editorSaveBtn.click(); return; }
            if (e.key === 'Escape') { e.preventDefault(); editorCancelBtn.click(); return; }
            if (e.key === 'Tab') {
                e.preventDefault();
                const start = editorTextarea.selectionStart;
                const end = editorTextarea.selectionEnd;
                editorTextarea.value = editorTextarea.value.substring(0, start) + '\t' + editorTextarea.value.substring(end);
                editorTextarea.selectionStart = editorTextarea.selectionEnd = start + 1;
            }
        });

        function updateCursorPos() {
            const text = editorTextarea.value;
            const pos = editorTextarea.selectionStart;
            let line = 1, col = 1;
            for (let i = 0; i < pos; i++) {
                if (text[i] === '\n') { line++; col = 1; } else col++;
            }
            editorCursorPos.textContent = 'Ln ' + line + ', Col ' + col;
        }
        editorTextarea.addEventListener('keyup', updateCursorPos);
        editorTextarea.addEventListener('click', updateCursorPos);

        function showExitHint(text) {
            const existing = document.querySelector('.exit-hint');
            if (existing) existing.remove();
            const hint = document.createElement('div');
            hint.className = 'exit-hint';
            hint.textContent = text;
            document.body.appendChild(hint);
            setTimeout(() => { if (hint.parentNode) hint.remove(); }, 3000);
        }

        function startMatrix() {
            matrixCanvas.width = window.innerWidth;
            matrixCanvas.height = window.innerHeight;
            const chars = '01ﾠﾡ￡￢￥￤￦￧￨￩￪￫￬￭｡｢｣､･ｦｧｨｩｪｫｬｭｮｯｰｱｲｳｴｵｶｷｸｹｺｻｼｽｾｿﾀﾁﾂﾃﾄﾅﾆﾇﾈﾉﾊﾋﾌﾍﾎﾏﾐﾑﾒﾓﾔﾕﾖﾗﾘﾙﾚﾛﾜﾝ1234567890ABCDEFGHIJKLMNOPQRSTUVWXYZ';
            const fontSize = 14;
            const columns = Math.floor(matrixCanvas.width / fontSize);
            const drops = [];
            for (let i = 0; i < columns; i++) drops[i] = Math.random() * -100;
            document.body.classList.add('matrix-active');
            matrixCanvas.style.display = 'block';
            showExitHint("Press any key / Click / ESC to exit Matrix");
            function draw() {
                matrixCtx.fillStyle = 'rgba(0, 0, 0, 0.05)';
                matrixCtx.fillRect(0, 0, matrixCanvas.width, matrixCanvas.height);
                matrixCtx.fillStyle = '#0f0';
                matrixCtx.font = fontSize + 'px monospace';
                for (let i = 0; i < drops.length; i++) {
                    matrixCtx.fillText(chars.charAt(Math.floor(Math.random() * chars.length)), i * fontSize, drops[i] * fontSize);
                    if (drops[i] * fontSize > matrixCanvas.height && Math.random() > 0.975) drops[i] = 0;
                    drops[i]++;
                }
            }
            matrixInterval = setInterval(draw, 33);
            const exitHandler = (e) => {
                if (['Escape', 'Enter', ' '].includes(e.key) || e.type === 'click') {
                    e.preventDefault();
                    stopMatrix();
                    document.removeEventListener('keydown', exitHandler);
                    document.removeEventListener('click', exitHandler);
                }
            };
            document.addEventListener('keydown', exitHandler);
            document.addEventListener('click', exitHandler);
        }
        function stopMatrix() {
            if (matrixInterval) { clearInterval(matrixInterval); matrixInterval = null; }
            matrixCanvas.style.display = 'none';
            document.body.classList.remove('matrix-active');
            input.focus();
        }
        function startCatEasterEgg() {
            catContainer.innerHTML = '';
            document.body.classList.add('cat-active');
            catContainer.style.display = 'block';
            showExitHint("Click anywhere or press any key to stop the cats 🐱");
            for(let i=0; i<10; i++) setTimeout(spawnCat, i * 50);
            catSpawnInterval = setInterval(spawnCat, 100);
            const exitHandler = (e) => {
                e.preventDefault?.();
                stopCatEasterEgg();
                document.removeEventListener('click', exitHandler);
                document.removeEventListener('keydown', exitHandler);
            };
            document.addEventListener('click', exitHandler);
            document.addEventListener('keydown', exitHandler);
        }
        function spawnCat() {
            const cat = document.createElement('div');
            cat.className = 'falling-cat';
            cat.textContent = CAT_EMOJIS[Math.floor(Math.random() * CAT_EMOJIS.length)];
            const left = Math.random() * 100;
            const size = Math.random() * 40 + 24;
            const duration = Math.random() * 2.5 + 2.5;
            const delay = Math.random() * 0.2;
            cat.style.left = `${left}%`;
            cat.style.fontSize = `${size}px`;
            cat.style.animationDuration = `${duration}s`;
            cat.style.animationDelay = `${delay}s`;
            cat.style.opacity = Math.random() * 0.3 + 0.7;
            cat.addEventListener('animationend', () => cat.remove());
            catContainer.appendChild(cat);
        }
        function stopCatEasterEgg() {
            if (catSpawnInterval) { clearInterval(catSpawnInterval); catSpawnInterval = null; }
            catContainer.innerHTML = '';
            document.body.classList.remove('cat-active');
            catContainer.style.display = 'none';
            input.focus();
        }
        function renderFortune(text) {
            const clean = text.trim().replace(/^"|"$/g, '');
            return `<span class="fortune-text"> ${clean}</span>\n`;
        }
        function formatBytes(bytes) {
            if (bytes < 1024) return bytes + ' B';
            if (bytes < 1024*1024) return (bytes/1024).toFixed(1) + ' KB';
            return (bytes/1024/1024).toFixed(1) + ' MB';
        }
        function handleDownloadStart(info) {
            window.dlBuffer = new Uint8Array(info.size);
            window.dlReceived = 0; window.dlFilename = info.filename;
            window.dlTotal = info.size; window.dlChunkSize = info.chunk_size || CHUNK_SIZE;
            lastDlProg = -1;
            output.textContent += `dl: Starting: ${info.filename} (${formatBytes(info.size)})\n`;
            output.scrollTop = output.scrollHeight;
            ws.send(`__DL_ACK__:0`);
        }
        function handleDownloadChunk(chunk) {
            if (!window.dlBuffer) return;
            try {
                const offset = chunk.index * (chunk.chunk_size || CHUNK_SIZE);
                const binary = atob(chunk.data);
                for (let i = 0; i < binary.length; i++) { window.dlBuffer[offset + i] = binary.charCodeAt(i); }
                window.dlReceived += chunk.size;
                let prog = chunk.progress || Math.floor(window.dlReceived * 100 / window.dlTotal);
                let step = Math.floor(prog / 5);
                if (step > lastDlProg) {
                    lastDlProg = step;
                    output.textContent += `dl: Progress: ${prog}%\n`;
                    output.scrollTop = output.scrollHeight;
                }
                ws.send(`__DL_ACK__:${chunk.index}`);
            } catch(e) {
                output.textContent += `dl: Error: ${e.message}\n`;
                output.scrollTop = output.scrollHeight;
                setTimeout(() => connect(), 1000);
            }
        }
        function handleDownloadEnd(info) {
            if (!window.dlBuffer) return;
            const blob = new Blob([window.dlBuffer]);
            const url = URL.createObjectURL(blob);
            const a = document.createElement('a');
            a.href = url; a.download = window.dlFilename;
            document.body.appendChild(a); a.click(); document.body.removeChild(a);
            URL.revokeObjectURL(url);
            output.textContent += `dl: Complete: ${window.dlFilename}\n`;
            output.scrollTop = output.scrollHeight;
            delete window.dlBuffer;
        }
        function startChunkedUpload(file) {
            const reader = new FileReader();
            const chunkSize = CHUNK_SIZE; let offset = 0;
            lastUlProg = -1; uploadComplete = false;
            output.textContent += `ul: Starting: ${file.name} (${formatBytes(file.size)})\n`;
            output.scrollTop = output.scrollHeight;
            ws.send(`__UPLOAD_START__:filename:${file.name},size:${file.size},chunk_size:${chunkSize}`);
            function sendNextChunk() {
                if (offset >= file.size) {
                    ws.send(`__UPLOAD_END__`);
                    if (!uploadComplete) {
                        output.textContent += `ul: Saving file...\n`;
                        output.scrollTop = output.scrollHeight;
                    }
                    return;
                }
                const end = Math.min(offset + chunkSize, file.size);
                const slice = file.slice(offset, end);
                reader.onload = (e) => {
                    const arr = new Uint8Array(e.target.result);
                    let binary = '';
                    for (let i = 0; i < arr.length; i++) binary += String.fromCharCode(arr[i]);
                    const b64 = btoa(binary);
                    ws.send(`__UPLOAD_CHUNK__:index:${Math.floor(offset/chunkSize)},${b64}`);
                    offset = end;
                    let prog = Math.floor(offset * 100 / file.size);
                    let step = Math.floor(prog / 5);
                    if (step > lastUlProg) {
                        lastUlProg = step;
                        output.textContent += `ul: Progress: ${prog}%\n`;
                        output.scrollTop = output.scrollHeight;
                    }
                    setTimeout(sendNextChunk, 50);
                };
                reader.readAsArrayBuffer(slice);
            }
            sendNextChunk();
        }
        function connect() {
            const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
            ws = new WebSocket(`${protocol}//${window.location.hostname}:81`);
            ws.onopen = () => {
                output.textContent += "=== ESP32-CINUX SYSTEM CONNECTED ===\n";
                output.textContent += "Type 'help' for command list\n\n";
                output.scrollTop = output.scrollHeight;
                syncTime();
            };
            ws.onmessage = (evt) => {
                try {
                    const data = JSON.parse(evt.data);
                    if (data.edit_start) { enterEditorMode(data.edit_start.filename, data.edit_start.content); return; }
                    if (data.dl_start) { handleDownloadStart(data.dl_start); return; }
                    if (data.dl_chunk) { handleDownloadChunk(data.dl_chunk); return; }
                    if (data.dl_end) { handleDownloadEnd(data.dl_end); return; }
                    if (data.ul_ack) {
                        if (data.ul_ack.status === 'error') {
                            output.textContent += `ul: Error: ${data.ul_ack.msg}\n`;
                            output.scrollTop = output.scrollHeight;
                        } else if (data.ul_ack.status === 'complete') {
                            uploadComplete = true;
                            output.textContent += `ul: Saved: ${data.ul_ack.written} bytes\n`;
                            output.scrollTop = output.scrollHeight;
                        }
                        return;
                    }
                    if (data.output) {
                        if(data.output.includes("edit: Type 'EOF' to save & exit")) isEditingMode = true;
                        if(data.output.includes("edit: File saved")) isEditingMode = false;
                        if (data.output.includes("__MATRIX_MODE__")) { startMatrix(); return; }
                        if (data.output.includes("__CAT_EASTER_EGG__")) { startCatEasterEgg(); return; }
                        if (data.prompt && data.prompt.includes("fortune")) output.textContent += renderFortune(data.output);
                        else output.textContent += data.output;
                        output.scrollTop = output.scrollHeight;
                    }
                    if (data.prompt) prompt.textContent = data.prompt;
                    if (data.clear) output.textContent = '';
                    if (data.download) {
                        const { filename, content } = data.download;
                        try {
                            const byteCharacters = atob(content);
                            const byteNumbers = new Array(byteCharacters.length);
                            for (let i = 0; i < byteCharacters.length; i++) byteNumbers[i] = byteCharacters.charCodeAt(i);
                            const blob = new Blob([new Uint8Array(byteNumbers)]);
                            const url = URL.createObjectURL(blob);
                            const a = document.createElement('a'); a.href = url; a.download = filename;
                            document.body.appendChild(a); a.click(); document.body.removeChild(a);
                            URL.revokeObjectURL(url);
                        } catch(e) { output.textContent += "[ERR] Download failed: " + e.message + "\n"; output.scrollTop = output.scrollHeight; }
                    }
                    if (data.www) { window.open(data.www, '_blank'); }
                    if (data.upload) fileInput.click();
                } catch (e) { output.textContent += "[ERR] Message parse failed\n"; output.scrollTop = output.scrollHeight; }
            };
            ws.onclose = () => { output.textContent += "\n=== CONNECTION LOST - RECONNECTING ===\n"; output.scrollTop = output.scrollHeight; setTimeout(connect, 2000); };
            ws.onerror = (err) => console.error("WebSocket error:", err);
        }
        fileInput.addEventListener('change', (e) => {
            const file = e.target.files[0];
            if (!file) return;
            startChunkedUpload(file);
            e.target.value = '';
        });
        function syncTime() { ws.send(`__SYNC_TIME__:${Math.floor(Date.now() / 1000)}`); }
        input.addEventListener('keydown', (e) => {
            if (isEditorMode) return;
            if (e.key === 'Enter') {
                const cmd = input.value;
                if(isEditingMode) output.textContent += cmd + '\n';
                else output.textContent += prompt.textContent + cmd + '\n';
                output.scrollTop = output.scrollHeight;
                if (cmd.trim()) ws.send(cmd);
                if (cmd.trim() && !document.body.classList.contains('matrix-active') && !document.body.classList.contains('cat-active')) {
                    cmdHistory.push(cmd); histPos = -1;
                }
                input.value = '';
            }
            if (e.key === 'ArrowUp') {
                e.preventDefault();
                if (cmdHistory.length > 0) {
                    histPos = Math.min(histPos + 1, cmdHistory.length - 1);
                    input.value = cmdHistory[cmdHistory.length - 1 - histPos];
                }
            }
            if (e.key === 'ArrowDown') {
                e.preventDefault();
                if (histPos > 0) { histPos--; input.value = cmdHistory[cmdHistory.length - 1 - histPos]; }
                else if (histPos === 0) { histPos = -1; input.value = ''; }
            }
            if (document.body.classList.contains('matrix-active') || document.body.classList.contains('cat-active')) e.stopPropagation();
        });
        document.querySelector('.screen').addEventListener('click', (e) => {
            if (!document.body.classList.contains('matrix-active') && !document.body.classList.contains('cat-active')) input.focus();
        });
        window.addEventListener('resize', () => {
            if (matrixCanvas.style.display !== 'none') { matrixCanvas.width = window.innerWidth; matrixCanvas.height = window.innerHeight; }
        });
        document.addEventListener('touchmove', (e) => {
            if (document.body.classList.contains('matrix-active') || document.body.classList.contains('cat-active')) e.preventDefault();
        }, { passive: false });
        connect();
    </script>
</body>
</html>
)rawliteral";

#endif
