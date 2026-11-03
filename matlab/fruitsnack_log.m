function fruitsnack_log(portName, varargin)
%FRUITSNAK_LOG  Live plot and log the FruitSnack UART telemetry tap.
%   fruitsnack_log("COM7")
%   fruitsnack_log("COM7", "Duration", 600, "Output", "runs/step_200C")
%
%   The firmware emits one line every 250 ms on the UART USB port:
%       ms,temp_c,setpoint_c,duty
%
%   Every received line is echoed to the command window and appended to
%   <name>.raw.txt, so the exact serial stream is visible while debugging.
%   The top plot shows process value and setpoint against time; the bottom
%   plot shows the raw heater duty against the same time axis.
%
%   Requires MATLAB R2019b+ and the Instrument Control Toolbox.

p = inputParser;
p.addRequired('portName');
p.addParameter('Baud', 921600, @(x) isnumeric(x) && isscalar(x) && x > 0);
p.addParameter('Duration', Inf, @(x) isnumeric(x) && isscalar(x) && x > 0);
p.addParameter('Output', "", @(x) ischar(x) || isstring(x));
p.parse(portName, varargin{:});

portName = char(p.Results.portName);
baud = double(p.Results.Baud);
runDuration = double(p.Results.Duration);
outputBase = char(p.Results.Output);

if isempty(strtrim(portName))
    error('fruitsnack_log:badPort', 'portName must not be empty.');
end
if isempty(strtrim(outputBase))
    outputBase = char("fruitsnack_" + string(datetime('now', 'Format', 'yyyyMMdd_HHmmss')));
end

outDir = fileparts(outputBase);
if ~isempty(outDir) && ~isfolder(outDir)
    mkdir(outDir);
end

csvPath = [outputBase '.csv'];
matPath = [outputBase '.mat'];
rawPath = [outputBase '.raw.txt'];

fprintf('FruitSnack logger: %s @ %d baud\n', portName, baud);
fprintf('Telemetry: ms,temp_c,setpoint_c,duty (4 Hz)\n');
fprintf('Raw lines: %s\n', rawPath);
fprintf('Press Ctrl+C to stop and save.\n');

rawFid = fopen(rawPath, 'w');
if rawFid < 0
    warning('fruitsnack_log:rawFile', ...
        'Could not open %s; raw lines will not be saved.', rawPath);
end

try
    s = serialport(portName, baud, 'Timeout', 2);
catch err
    if rawFid > 0
        fclose(rawFid);
    end
    error('fruitsnack_log:openFailed', ...
        'Could not open %s: %s\nClose pio device monitor or any other program using the port.', ...
        portName, err.message);
end

chunk = 4096;
t = NaN(1, chunk);
temp = NaN(1, chunk);
setpoint = NaN(1, chunk);
duty = NaN(1, chunk);
n = 0;
t0 = NaN;
emptyReads = 0;
saved = false;
startedAt = datetime('now');

% The cleanup object is created only after all variables finish() needs exist.
cleanupObj = onCleanup(@finish); %#ok<NASGU>
configureTerminator(s, 'LF');

% If a previous Simulink run left the firmware in binary mode, return it to
% ASCII before the line-based reader starts.
write(s, uint8(3), 'uint8');
pause(0.1);

fig = figure('Name', 'FruitSnack telemetry', 'NumberTitle', 'off');
tl = tiledlayout(fig, 2, 1, 'TileSpacing', 'compact', 'Padding', 'compact');

ax1 = nexttile(tl);
hold(ax1, 'on');
grid(ax1, 'on');
hPV = plot(ax1, NaN, NaN, 'LineWidth', 1.5);
hSP = plot(ax1, NaN, NaN, '--', 'LineWidth', 1.2);
ylabel(ax1, 'Temperature (C)');
legend(ax1, [hPV hSP], {'Process value', 'Setpoint'}, 'Location', 'best');

ax2 = nexttile(tl);
hold(ax2, 'on');
grid(ax2, 'on');
hDuty = plot(ax2, NaN, NaN, 'LineWidth', 1.2);
ylabel(ax2, 'Duty (0-255)');
xlabel(ax2, 'Time (s)');

linkaxes([ax1 ax2], 'x');

runTimer = tic;
lastDraw = tic;

while toc(runTimer) < runDuration
    try
        line = readline(s);
    catch
        emptyReads = emptyReads + 1;
        if mod(emptyReads, 5) == 0
            fprintf('No data: %d consecutive empty reads/timeouts.\n', emptyReads);
        end
        if emptyReads >= 20
            warning('fruitsnack_log:noData', ...
                ['No telemetry for %d reads. Use the UART USB port (GPIO43/44), ' ...
                 'not the native USB port.'], emptyReads);
            break;
        end
        continue;
    end

    line = strtrim(char(line));
    if isempty(line)
        emptyReads = emptyReads + 1;
        if emptyReads >= 20
            warning('fruitsnack_log:noData', ...
                ['No telemetry for %d reads. Use the UART USB port (GPIO43/44), ' ...
                 'not the native USB port.'], emptyReads);
            break;
        end
        continue;
    end
    emptyReads = 0;

    fprintf('RX: %s\n', line);
    if rawFid > 0
        fprintf(rawFid, '%s\n', line);
    end

    parts = strsplit(line, ',');
    if numel(parts) ~= 4
        fprintf('  [ignored: %d field(s)]\n', numel(parts));
        continue;
    end

    v = str2double(parts);
    if any(~isfinite(v))
        fprintf('  [ignored: non-numeric field]\n');
        continue;
    end

    if isnan(t0)
        t0 = v(1) / 1000;
    end
    n = n + 1;
    if n > numel(t)
        t(end+1:end+chunk) = NaN;
        temp(end+1:end+chunk) = NaN;
        setpoint(end+1:end+chunk) = NaN;
        duty(end+1:end+chunk) = NaN;
    end

    t(n) = v(1) / 1000 - t0;
    temp(n) = v(2);
    setpoint(n) = v(3);
    duty(n) = v(4);

    if mod(n, 10) == 1
        fprintf('  sample %d: t=%.2f s temp=%.2f C sp=%.2f C duty=%.0f\n', ...
            n, t(n), temp(n), setpoint(n), duty(n));
    end

    if toc(lastDraw) >= 0.25
        if ~ishghandle(hPV)
            break;
        end
        set(hPV, 'XData', t(1:n), 'YData', temp(1:n));
        set(hSP, 'XData', t(1:n), 'YData', setpoint(1:n));
        set(hDuty, 'XData', t(1:n), 'YData', duty(1:n));
        drawnow limitrate;
        lastDraw = tic;
    end
end

if n > 0 && ishghandle(hPV)
    set(hPV, 'XData', t(1:n), 'YData', temp(1:n));
    set(hSP, 'XData', t(1:n), 'YData', setpoint(1:n));
    set(hDuty, 'XData', t(1:n), 'YData', duty(1:n));
    drawnow;
end

    function finish()
        if saved
            return;
        end
        saved = true;

        if ~isempty(s)
            try
                delete(s);
            catch
            end
        end
        if rawFid > 0
            try
                fclose(rawFid);
            catch
            end
        end

        if n == 0
            fprintf('No telemetry samples captured.\n');
            return;
        end

        T = table(t(1:n).', temp(1:n).', setpoint(1:n).', duty(1:n).', ...
            'VariableNames', {'time_s', 'temp_c', 'setpoint_c', 'duty'});
        meta = struct( ...
            'port', portName, ...
            'baud', baud, ...
            'started', startedAt, ...
            'samples', n, ...
            'sample_period_ms', 250, ...
            'duty_units', 'raw LEDC 0-255', ...
            'time_units', 'seconds since first sample');

        writetable(T, csvPath);
        save(matPath, 'T', 'meta');
        fprintf('Saved %d samples:\n  %s\n  %s\n', n, csvPath, matPath);
    end
end
