#ifndef WEB_PAGE_H
#define WEB_PAGE_H

const char* index_html = 
"<!DOCTYPE html>"
"<html>"
"<head>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<title>ESP32 BMS Flasher</title>"
    "<style>"
        "* { margin: 0; padding: 0; box-sizing: border-box; }"
        "body { font-family: 'Segoe UI', sans-serif; background: #1a1a2e; color: #fff; padding: 20px; }"
        ".container { max-width: 800px; margin: 0 auto; }"
        "h1 { text-align: center; color: #00ff88; margin-bottom: 20px; font-size: 24px; }"
        ".card { background: #252525; padding: 20px; border-radius: 10px; border: 1px solid #333; margin-bottom: 20px; box-shadow: 0 4px 6px rgba(0,0,0,0.3); }"
        "h2 { font-size: 18px; border-bottom: 1px solid #444; padding-bottom: 10px; margin-bottom: 15px; color: #ccc; }"
        
        "textarea { width: 100%; padding: 12px; background: #161625; color: #0f0; border: 1px solid #444; border-radius: 5px; font-family: monospace; resize: vertical; margin-top: 10px; }"
        
        /* NEW FILE INPUT STYLING */
        ".file-input-wrapper { display: flex; align-items: center; gap: 10px; margin-bottom: 10px; }"
        "input[type='file'] { background: #333; color: #fff; padding: 8px; border-radius: 5px; border: 1px solid #555; width: 100%; cursor: pointer; }"
        
        ".btn { background: #00ff88; color: #000; padding: 12px; border: none; border-radius: 5px; cursor: pointer; width: 100%; font-weight: bold; font-size: 16px; margin-top: 15px; transition: 0.2s; }"
        ".btn:hover { background: #00cc6a; }"
        ".btn:disabled { background: #555; color: #888; cursor: not-allowed; }"
        
        ".progress-bg { width: 100%; background-color: #444; border-radius: 5px; margin-top: 15px; height: 30px; overflow: hidden; position: relative; }"
        ".progress-fill { width: 0%; height: 100%; background-color: #00ff88; transition: width 0.5s; }"
        ".progress-text { position: absolute; width: 100%; height: 100%; top: 0; left: 0; display: flex; align-items: center; justify-content: center; color: #fff; font-weight: bold; mix-blend-mode: difference; }"
        ".status-row { display: flex; justify-content: space-between; margin-top: 10px; font-family: monospace; color: #aaa; font-size: 14px; }"
        ".highlight { color: #00ff88; }"
    "</style>"
"</head>"
"<body>"
    "<div class='container'>"
        "<h1>Vega BMS Updater</h1>"
        
        "<div class='card'>"
            "<h2>1. Upload Firmware as txt or paste in the box</h2>"
            
            ""
            "<div class='file-input-wrapper'>"
                "<input type='file' id='fileInput' accept='.txt,.hex,.csv'>"
            "</div>"
            
            "<textarea id='hexInput' rows='8' placeholder='Select a file above OR Paste Data Here...'></textarea>"
            "<button class='btn' id='uploadBtn' onclick='uploadFirmware()'>Upload & Verify</button>"
            "<div class='status-row'>"
                "<span>Status: <span id='uploadStatus' class='highlight'>Idle</span></span>"
                "<span>RAM Usage: <span id='ramSize'>0</span> bytes</span>"
            "</div>"
        "</div>"

        "<div class='card'>"
            "<h2>2. Update the BMS</h2>"
            "<p style='color:#888; font-size: 13px; margin-bottom: 10px;'>Ensure CAN bus is connected before starting.</p>"
            "<button class='btn' id='flashBtn' onclick='startFlash()' disabled>Start Update</button>"
            
            "<div class='progress-bg'>"
                "<div id='progressBar' class='progress-fill'></div>"
                "<div id='progressText' class='progress-text'>0%</div>"
            "</div>"
            
            "<div class='status-row'>"
                "<span>System: <span id='sysState' class='highlight'>Idle</span></span>"
                "<span>Progress: <span id='sentBytes'>0</span> / <span id='totalBytes'>0</span></span>"
            "</div>"
        "</div>"
    "</div>"

    "<script>"
        "let isFlashing = false;"
        "let pollInterval = null;"
        
        // --- FILE READER LOGIC ---
        "document.getElementById('fileInput').addEventListener('change', function(e) {"
            "let file = e.target.files[0];"
            "if (!file) return;"
            
            "let reader = new FileReader();"
            "reader.onload = function(e) {"
                "document.getElementById('hexInput').value = e.target.result;"
                "alert('File loaded! Click Upload & Verify to proceed.');"
            "};"
            "reader.readAsText(file);"
        "});"
        // -------------------------

        "function cleanHex(input) {"
            "let clean = input.replace(/0x/gi, '');"
            "clean = clean.replace(/[^0-9A-Fa-f]/g, '');"
            "return clean;"
        "}"

        "function uploadFirmware() {"
            "let raw = document.getElementById('hexInput').value;"
            "let hex = cleanHex(raw);"
            
            "if(hex.length % 2 !== 0 || hex.length === 0) { alert('Invalid Data (Odd length)! Check your input.'); return; }"
            "if(hex.length > 200000) { alert('File too large (>100KB binary)!'); return; }"
            
            "document.getElementById('uploadBtn').disabled = true;"
            "document.getElementById('uploadStatus').innerText = 'Uploading...';"
            
            "fetch('/api/upload', { method: 'POST', body: hex })"
            ".then(r => { if(r.ok) return r.json(); throw new Error(r.statusText); })"
            ".then(d => {"
                "document.getElementById('uploadStatus').innerText = 'Verified';"
                "document.getElementById('ramSize').innerText = d.size;"
                "document.getElementById('flashBtn').disabled = false;"
                "document.getElementById('uploadBtn').disabled = false;"
                "document.getElementById('totalBytes').innerText = d.size;"
                "alert('Firmware loaded into RAM successfully.');"
            "}).catch(e => {"
                "document.getElementById('uploadStatus').innerText = 'Error';"
                "document.getElementById('uploadBtn').disabled = false;"
                "alert('Upload Failed: ' + e);"
            "});"
        "}"

        "function startFlash() {"
            "if(!confirm('Start BMS Update? Do not power off.')) return;"
            
            "isFlashing = true;"
            "document.getElementById('flashBtn').disabled = true;"
            "document.getElementById('uploadBtn').disabled = true;"
            "document.getElementById('hexInput').disabled = true;"
            "document.getElementById('fileInput').disabled = true;" // Disable file input too
            "document.getElementById('sysState').innerText = 'Starting...';"
            
            "fetch('/api/flash', { method: 'POST' })"
            ".then(r => { if(r.ok) return r.json(); throw new Error('Busy'); })"
            ".then(d => {"
                "if(pollInterval) clearInterval(pollInterval);"
                "pollInterval = setInterval(pollStatus, 500);" 
            "}).catch(e => {"
                "alert('Could not start flash: ' + e);"
                "isFlashing = false;"
                "document.getElementById('flashBtn').disabled = false;"
                "document.getElementById('uploadBtn').disabled = false;"
                "document.getElementById('hexInput').disabled = false;"
                "document.getElementById('fileInput').disabled = false;"
            "});"
        "}"

        "function pollStatus() {"
            "fetch('/api/status')"
            ".then(r => r.json())"
            ".then(d => {"
                "document.getElementById('sysState').innerText = d.status;"
                "document.getElementById('sentBytes').innerText = d.sent;"
                "document.getElementById('totalBytes').innerText = d.total;"
                
                "let pct = 0;"
                "if(d.total > 0) pct = Math.round((d.sent / d.total) * 100);"
                "if(pct > 100) pct = 100;"
                
                "document.getElementById('progressBar').style.width = pct + '%';"
                "document.getElementById('progressText').innerText = pct + '%';"

                "if (d.busy === false && isFlashing) {"
                    "clearInterval(pollInterval);"
                    "isFlashing = false;"
                    "document.getElementById('uploadBtn').disabled = false;"
                    "document.getElementById('hexInput').disabled = false;"
                    "document.getElementById('fileInput').disabled = false;"
                    "document.getElementById('flashBtn').disabled = true;"
                    
                    "if(d.status.includes('Success')) alert('Update Complete Successfully!');"
                    "else alert('Update Failed: ' + d.status);"
                "}"
            "}).catch(e => console.log('Poll error', e));"
        "}"
    "</script>"
"</body>"
"</html>";

#endif