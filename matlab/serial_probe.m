function serial_probe(portName)
%SERIAL_PROBE  Verify the FruitSnack binary UART stream without Simulink.
%
%   serial_probe("COM7")
%
%   Opens the port, sends STX (0x02) repeatedly until a 0xAA 0x55 frame is
%   found, decodes the four float32 values, prints three frames, and sends ETX
%   (0x03) on exit.  Retrying STX matters because opening the port can reset
%   the ESP32, which boots in ASCII mode.
%
%   This isolates the firmware/protocol from the Simulink model: if it prints
%   values, the device is streaming correctly and any remaining problem is in
%   the model or its port selection.  Close every monitor and logger first.

if nargin < 1 || isempty(portName)
    portName = input('UART COM port (e.g. COM9): ', 's');
end
portName = strtrim(char(portName));
if isempty(portName)
    error('serial_probe:noPort', 'No port name supplied.');
end

try
    s = serialport(portName, 921600, "Timeout", 0.5);
catch err
    available = listPorts();
    error('serial_probe:open', ...
        ['Could not open %s: %s\n' ...
         'Close any serial monitor, logger, or Simulink model that owns the port.\n' ...
         'Available ports: %s'], ...
        portName, err.message, available);
end
cleanupObj = onCleanup(@() cleanupPort(s));

% Keep the ESP32 auto-reset lines deasserted.
try
    setDTR(s, false);
catch
end
try
    setRTS(s, false);
catch
end
try
    flush(s);
catch
end

fprintf('Opened %s @ 921600. Looking for 0xAA 0x55 frames...\n', portName);

buf = uint8([]);
frames = 0;
stxCount = 0;
lastStx = -inf;
start = tic;

while toc(start) < 12
    elapsed = toc(start);
    if elapsed - lastStx >= 0.5
        try
            write(s, uint8(2), "uint8");
        catch
        end
        lastStx = elapsed;
        stxCount = stxCount + 1;
        if stxCount <= 2 || mod(stxCount, 10) == 0
            fprintf('  sent STX (%d) at %.1f s\n', stxCount, elapsed);
        end
    end

    try
        n = s.NumBytesAvailable;
        if n > 0
            data = read(s, n, "uint8");
            if ~isempty(data)
                buf = [buf, data(:)']; %#ok<AGROW>
            end
        end
    catch err
        fprintf('Read failed: %s\n', err.message);
        break;
    end

    while numel(buf) >= 18
        idx = find(buf(1:end-1) == uint8(170) & ...
                   buf(2:end) == uint8(85), 1);
        if isempty(idx)
            % No header: keep one byte in case the next byte completes a
            % split header, and discard the rest of the stale data.
            buf = buf(max(1, numel(buf) - 1):end);
            break;
        end
        if idx > 1
            buf(1:idx-1) = [];
        end
        if numel(buf) < 18
            break;
        end

        values = typecast(buf(3:18), "single");
        frames = frames + 1;
        fprintf('  seconds=%.2f  temp=%.2f C  setpoint=%.2f C  duty=%.0f\n', ...
            values(1), values(2), values(3), values(4));
        buf(1:18) = [];
    end

    if frames >= 3
        break;
    end
    pause(0.02);
end

if frames == 0
    warning('serial_probe:noFrame', ...
        ['No 0xAA 0x55 frame found on %s in 12 s.\n' ...
         'Check: (1) this is the UART USB port, not native USB; ' ...
         '(2) the 18-byte binary firmware is flashed; ' ...
         '(3) no monitor or logger owns the port; (4) baud is 921600.\n' ...
         'Available ports: %s'], ...
        portName, listPorts());
else
    fprintf('Received %d frame(s) in %.1f s. Firmware stream is good.\n', ...
        frames, toc(start));
end
end

function available = listPorts()
available = '(none)';
try
    ports = cellstr(serialportlist("available"));
    if ~isempty(ports)
        available = strjoin(ports, ', ');
    end
catch
end
end

function cleanupPort(s)
try
    write(s, uint8(3), "uint8");
catch
end
try
    delete(s);
catch
end
end
