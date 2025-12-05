#ifndef WEB_PAGE_H
#define WEB_PAGE_H

const char index_html[] = R"rawliteral(
<!DOCTYPE HTML><html>
<head>
  <title>ESP32 BMS OTA</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { font-family: Arial; text-align: center; margin: 20px; }
    #progressBar { width: 100%; background-color: #ddd; }
    #bar { width: 0%; height: 30px; background-color: #4CAF50; text-align: center; line-height: 30px; color: white; }
    .btn { background-color: #008CBA; border: none; color: white; padding: 15px 32px; font-size: 16px; margin: 10px; cursor: pointer; }
  </style>
</head>
<body>
  <h2>BMS Firmware Updater</h2>
  
  <input type="file" id="firmware_file" accept=".bin"><br><br>
  <button class="btn" onclick="uploadFirmware()">1. Upload & Verify</button>
  <button class="btn" onclick="flashFirmware()">2. Flash to BMS</button>
  
  <br><br>
  <div id="progressBar"><div id="bar">0%</div></div>
  <p id="status">Status: Idle</p>

  <script>
    // --- SHA-256 IMPLEMENTATION (Pure JS for HTTP Compatibility) ---
    var sha256=function a(b){function c(a,b){return a>>>b|a<<32-b}for(var d,e,f=Math.pow,g=f(2,32),h="length",i="",j=[],k=8*b[h],l=a.h=a.h||[],m=a.k=a.k||[],n=m[h],o={},p=2;64>n;p++)if(!o[p]){for(d=0;313>d;d+=p)o[d]=p;l[n]=f(p,.5)*g|0,m[n++]=f(p,1/3)*g|0}for(b+="\x80";b[h]%64-56;)b+="\x00";for(d=0;d<b[h];d++){if(e=b.charCodeAt(d),e>>8)return;j[d>>2]|=e<<(3-d)%4*8}for(j[j[h]]=k/g|0,j[j[h]]=k,e=0;e<j[h];){var q=l.slice(d=0),r=j.slice(e,e+=16),s=q;for(a.h=q;64>d;d++)q=s[7]+(c(d=s[4],6)^c(d,11)^c(d,25))+(d&s[5]^~d&s[6])+m[d]+(r[d]=16>d?r[d]:r[d-2]+(c(d=r[d-15],7)^c(d,18)^d>>>3)+r[d-7]+(c(d=r[d-2],17)^c(d,19)^d>>>10)|0),s=[q+((c(d=s[0],2)^c(d,13)^c(d,22))+(d&s[1]^d&s[2]^s[1]&s[2]))|0].concat(s),s[4]=s[4]+q|0;for(d=0;8>d;d++)l[d]=l[d]+s[d]|0}for(d=0;8>d;d++)for(e=3;e+1;e--){var t=l[d]>>8*e&255;i+=(16>t?0:"")+t.toString(16)}return i};
    // ---------------------------------------------------------------

    function updateStatus(msg) {
      document.getElementById("status").innerText = "Status: " + msg;
    }

    // 1. READ FILE -> HASH -> UPLOAD
    function uploadFirmware() {
      var fileInput = document.getElementById('firmware_file');
      var file = fileInput.files[0];
      if (!file) { alert("Please select a .bin file"); return; }

      updateStatus("Reading file...");
      
      var reader = new FileReader();
      reader.onload = function(e) {
        var rawData = e.target.result; // This is an ArrayBuffer
        
        // Convert ArrayBuffer to Binary String for the SHA256 function
        var binaryString = "";
        var bytes = new Uint8Array(rawData);
        var len = bytes.byteLength;
        for (var i = 0; i < len; i++) {
          binaryString += String.fromCharCode(bytes[i]);
        }

        // Calculate Hash locally
        updateStatus("Calculating SHA-256...");
        var hashHex = sha256(binaryString);
        console.log("Client Hash:", hashHex);

        // Prepare Hex String for Upload (Your existing logic requirement)
        var hexPayload = "";
        for (var i = 0; i < len; i++) {
            var hex = bytes[i].toString(16);
            if(hex.length < 2) hex = "0" + hex;
            hexPayload += hex;
        }

        // Send
        updateStatus("Uploading...");
        var xhr = new XMLHttpRequest();
        xhr.open("POST", "/api/upload", true);
        
        // !!! VITAL HEADER !!!
        xhr.setRequestHeader("X-Payload-SHA256", hashHex); 
        
        xhr.onload = function() {
          if (xhr.status === 200) {
            var resp = JSON.parse(xhr.responseText);
            updateStatus("Verified! Size: " + resp.size + " bytes. Ready to Flash.");
            document.getElementById("bar").style.width = "100%";
            document.getElementById("bar").innerHTML = "100%";
          } else {
            updateStatus("Error: " + xhr.statusText);
            alert("Upload Failed: " + xhr.responseText);
          }
        };
        
        xhr.send(hexPayload);
      };
      reader.readAsArrayBuffer(file);
    }

    // 2. TRIGGER FLASH
    function flashFirmware() {
      var xhr = new XMLHttpRequest();
      xhr.open("POST", "/api/flash", true);
      xhr.onload = function() {
        if (xhr.status === 200) {
          updateStatus("Flashing Started...");
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