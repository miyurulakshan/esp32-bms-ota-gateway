#ifndef WEB_PAGE_H
#define WEB_PAGE_H

const char index_html[] = R"rawliteral(
<!DOCTYPE HTML><html>
<head>
  <title>ESP32 BMS OTA</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { font-family: Arial, sans-serif; text-align: center; margin: 40px; }
    h2 { color: #333; }
    #progressBar { width: 100%; background-color: #f1f1f1; border-radius: 5px; margin-top: 20px; }
    #bar { width: 0%; height: 30px; background-color: #4CAF50; border-radius: 5px; text-align: center; line-height: 30px; color: white; transition: width 0.3s; }
    .btn { background-color: #008CBA; border: none; color: white; padding: 15px 32px; font-size: 16px; margin: 10px; cursor: pointer; border-radius: 4px; }
    .btn:hover { background-color: #007399; }
    .btn:disabled { background-color: #ccc; cursor: not-allowed; }
    input[type=file] { padding: 10px; border: 1px solid #ccc; border-radius: 4px; }
  </style>
</head>
<body>
  <h2>BMS Firmware Updater</h2>
  <p>Select your Firmware Text File (.txt / .hex)</p>
  
  <input type="file" id="firmware_file" accept=".txt,.hex"><br><br>
  
  <button class="btn" id="btnUpload" onclick="uploadFirmware()">1. Upload & Verify</button>
  <button class="btn" id="btnFlash" onclick="flashFirmware()" disabled>2. Flash to BMS</button>
  
  <div id="progressBar"><div id="bar">0%</div></div>
  <p id="status">Status: Idle</p>

  <script>
    // --- SHA-256 IMPLEMENTATION (Pure JS) ---
    var sha256=function a(b){function c(a,b){return a>>>b|a<<32-b}for(var d,e,f=Math.pow,g=f(2,32),h="length",i="",j=[],k=8*b[h],l=a.h=a.h||[],m=a.k=a.k||[],n=m[h],o={},p=2;64>n;p++)if(!o[p]){for(d=0;313>d;d+=p)o[d]=p;l[n]=f(p,.5)*g|0,m[n++]=f(p,1/3)*g|0}for(b+="\x80";b[h]%64-56;)b+="\x00";for(d=0;d<b[h];d++){if(e=b.charCodeAt(d),e>>8)return;j[d>>2]|=e<<(3-d)%4*8}for(j[j[h]]=k/g|0,j[j[h]]=k,e=0;e<j[h];){var q=l.slice(d=0),r=j.slice(e,e+=16),s=q;for(a.h=q;64>d;d++)q=s[7]+(c(d=s[4],6)^c(d,11)^c(d,25))+(d&s[5]^~d&s[6])+m[d]+(r[d]=16>d?r[d]:r[d-2]+(c(d=r[d-15],7)^c(d,18)^d>>>3)+r[d-7]+(c(d=r[d-2],17)^c(d,19)^d>>>10)|0),s=[q+((c(d=s[0],2)^c(d,13)^c(d,22))+(d&s[1]^d&s[2]^s[1]&s[2]))|0].concat(s),s[4]=s[4]+q|0;for(d=0;8>d;d++)for(e=3;e+1;e--){var t=l[d]>>8*e&255;i+=(16>t?0:"")+t.toString(16)}return i};
    // ----------------------------------------

    function updateStatus(msg) {
      document.getElementById("status").innerText = "Status: " + msg;
    }

    // Helper: Hex String to Binary String (for hashing)
    function hexToBinaryString(hex) {
        var str = '';
        for (var i = 0; i < hex.length; i += 2) {
            str += String.fromCharCode(parseInt(hex.substr(i, 2), 16));
        }
        return str;
    }

    // 1. READ TEXT -> CLEAN -> HASH -> UPLOAD
    function uploadFirmware() {
      var fileInput = document.getElementById('firmware_file');
      var file = fileInput.files[0];
      if (!file) { alert("Please select a file"); return; }

      document.getElementById("btnUpload").disabled = true;
      updateStatus("Reading file...");
      
      var reader = new FileReader();
      
      // Changed to readAsText for .txt/.hex files
      reader.onload = function(e) {
        var rawText = e.target.result;
        
        // 1. CLEANUP: Remove any whitespace, newlines, or carriage returns
        // This ensures we have a pure stream of "AABB01..."
        var cleanHex = rawText.replace(/[^0-9A-Fa-f]/g, '');

        if (cleanHex.length % 2 !== 0) {
            alert("Error: Hex string length is odd. Corrupted file?");
            document.getElementById("btnUpload").disabled = false;
            return;
        }

        // 2. CONVERT TO BINARY STRING FOR HASHING
        // We must hash the BINARY values, not the Hex ASCII characters
        updateStatus("Calculating SHA-256...");
        var binaryString = hexToBinaryString(cleanHex);
        var hashHex = sha256(binaryString);
        
        console.log("Clean Hex Length:", cleanHex.length);
        console.log("Client Hash:", hashHex);

        // 3. UPLOAD THE CLEAN HEX STRING
        updateStatus("Uploading...");
        var xhr = new XMLHttpRequest();
        xhr.open("POST", "/api/upload", true);
        
        // Header matches the Binary Hash
        xhr.setRequestHeader("X-Payload-SHA256", hashHex); 
        
        xhr.onload = function() {
          document.getElementById("btnUpload").disabled = false;
          if (xhr.status === 200) {
            var resp = JSON.parse(xhr.responseText);
            updateStatus("Verified! Size: " + resp.size + " bytes. Ready to Flash.");
            document.getElementById("bar").style.width = "100%";
            document.getElementById("bar").innerHTML = "100%";
            document.getElementById("btnFlash").disabled = false;
          } else {
            updateStatus("Error: " + xhr.statusText);
            alert("Upload Failed: " + xhr.responseText);
          }
        };
        
        xhr.send(cleanHex);
      };
      
      reader.readAsText(file);
    }

    function flashFirmware() {
      var xhr = new XMLHttpRequest();
      xhr.open("POST", "/api/flash", true);
      xhr.onload = function() {
        if (xhr.status === 200) {
          updateStatus("Flashing Started...");
          document.getElementById("btnFlash").disabled = true;
          startPolling();
        } else {
          alert("Failed to start flash");
        }
      };
      xhr.send();
    }

    function startPolling() {
      var interval = setInterval(function() {
        var xhr = new XMLHttpRequest();
        xhr.open("GET", "/api/status", true);
        xhr.onload = function() {
          if (xhr.status === 200) {
            var json = JSON.parse(xhr.responseText);
            updateStatus(json.status);
            
            if (json.total > 0) {
              var percent = (json.sent / json.total) * 100;
              document.getElementById("bar").style.width = percent + "%";
              document.getElementById("bar").innerHTML = Math.round(percent) + "%";
            }

            if (json.busy === false && json.sent >= json.total && json.total > 0) {
              clearInterval(interval);
              alert("Update Complete!");
              document.getElementById("btnFlash").disabled = false;
            }
          }
        };
        xhr.send();
      }, 1000);
    }
  </script>
</body>
</html>
)rawliteral";

#endif