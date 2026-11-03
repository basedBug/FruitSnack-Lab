function build_live_scope(portName, varargin)
%BUILD_LIVE_SCOPE  Live Simulink scopes for the FruitSnack binary telemetry.
%
%   build_live_scope("COM7")
%   build_live_scope("COM7", "SampleTime", 0.25, "TimeSpan", 120)
%   build_live_scope("COM7", "Save", true)
%
%   Requires Simulink and Instrument Control Toolbox (for serialport).  This
%   does not use the instrumentblks library; the serial port is wrapped in
%   FruitSnackSerialSource.m and used through a standard MATLAB System block.
%
%   The model sends STX (0x02) when the simulation starts.  The firmware then
%   sends 18-byte frames:
%
%       uint8 0xAA, uint8 0x55,
%       single seconds, single temp_c, single setpoint_c, single duty
%
%   at 4 Hz.  The receiver scans for the AA 55 header, reads the 16-byte
%   payload, and typecasts it to four singles.  ETX (0x03) is sent when the
%   simulation stops, restoring the ASCII telemetry used by fruitsnack_log.
%
%   Simulation pacing is enabled so simulation time tracks wall time.  Without
%   it the non-blocking source lets Simulink run far faster than the 4 Hz
%   device, and the scopes appear empty even when frames are arriving.

if nargin < 1 || isempty(portName)
    error('build_live_scope:port', 'Pass the serial port name, for example "COM7".');
end
portName = char(portName);

p = inputParser;
p.addParameter('SampleTime', 0.25, @(x) isnumeric(x) && isscalar(x) && x > 0);
p.addParameter('TimeSpan', 120, @(x) isnumeric(x) && isscalar(x) && x > 0);
p.addParameter('Save', false, @(x) islogical(x) || isnumeric(x));
p.parse(varargin{:});
dt = p.Results.SampleTime;
timeSpan = p.Results.TimeSpan;

thisDir = fileparts(mfilename('fullpath'));
addpath(thisDir, '-begin');

clsFile = which('FruitSnackSerialSource');
if isempty(clsFile)
    error('build_live_scope:classMissing', ...
        'FruitSnackSerialSource.m is not on the MATLAB path. Run this from the folder containing both files.');
end
fprintf('Using class: %s\n', clsFile);
expected = fullfile(thisDir, 'FruitSnackSerialSource.m');
if ~strcmpi(clsFile, expected)
    warning('build_live_scope:classShadow', ...
        ['MATLAB is using %s, not %s. Close the model, fix the MATLAB path, ' ...
         'and run clear classes (or restart MATLAB) before regenerating.'], ...
        clsFile, expected);
end

% Fail early rather than opening a port that is not the device.  If the port
% list is unavailable, let serialport report the real error at setup.
listOk = false;
available = {};
try
    available = cellstr(serialportlist("available"));
    listOk = true;
catch
end
if listOk
    if isempty(available)
        warning('build_live_scope:noPorts', ...
            'serialportlist found no serial ports. %s may fail to open.', portName);
    else
        fprintf('Available serial ports: %s\n', strjoin(available, ', '));
        if ~any(strcmpi(available, portName))
            error('build_live_scope:portMissing', ...
                'Port %s is not available. Available ports: %s', ...
                portName, strjoin(available, ', '));
        end
    end
else
    warning('build_live_scope:portList', ...
        'Could not list serial ports. Continuing; serialport will report if %s cannot be opened.', ...
        portName);
end

mdl = 'fruitsnack_live_scope';
if bdIsLoaded(mdl)
    close_system(mdl, 0);
end
new_system(mdl);
open_system(mdl);

% The MATLAB System block hosts FruitSnackSerialSource.  Its public
% properties (Port, Baud, Timeout) appear as block parameters.
add_block('simulink/User-Defined Functions/MATLAB System', [mdl '/Source'], ...
    'Position', [30 90 190 190]);
set_param([mdl '/Source'], 'System', 'FruitSnackSerialSource');

% serialport is MATLAB-only; force interpreted execution before the model
% compiles, otherwise Simulink tries to generate C and rejects the block.
if ~setIf([mdl '/Source'], {'SimulateUsing'}, 'Interpreted execution')
    if ~setIf([mdl '/Source'], {'SimulateUsing'}, 'Interpreted Execution')
        error('build_live_scope:simMode', ...
            'Could not set SimulateUsing. Open the Source block and select "Interpreted execution".');
    end
end
try
    simMode = char(get_param([mdl '/Source'], 'SimulateUsing'));
    if ~contains(lower(simMode), 'interpreted')
        error('build_live_scope:simModeValue', ...
            'Source block is set to "%s", not interpreted execution.', simMode);
    end
catch err
    if strcmp(err.identifier, 'build_live_scope:simModeValue')
        rethrow(err);
    end
    warning('build_live_scope:simModeRead', ...
        'Could not read SimulateUsing back; verify it in the Source block dialog.');
end

if ~setIf([mdl '/Source'], {'Port'}, portName)
    error('build_live_scope:portParam', ...
        'Could not set Port to %s. Open the Source block and set it manually.', portName);
end
try
    actualPort = char(get_param([mdl '/Source'], 'Port'));
    fprintf('Source block port: %s\n', actualPort);
    if ~strcmpi(actualPort, portName)
        error('build_live_scope:portMismatch', ...
            'Source block Port is "%s", expected "%s".', actualPort, portName);
    end
catch err
    if strcmp(err.identifier, 'build_live_scope:portMismatch')
        rethrow(err);
    end
    warning('build_live_scope:portRead', ...
        'Could not read the Source port back; verify it in the block dialog.');
end

setIf([mdl '/Source'], {'Baud'}, '921600');
setIf([mdl '/Source'], {'Timeout'}, '2');
setIf([mdl '/Source'], {'SampleTime'}, num2str(dt));

add_block('simulink/Signal Routing/Demux', [mdl '/Demux'], ...
    'Position', [250 110 270 190], 'Outputs', '4');

add_block('simulink/Sinks/Scope', [mdl '/Temperature'], ...
    'Position', [330 70 380 130]);
set_param([mdl '/Temperature'], 'NumInputPorts', '2');
add_block('simulink/Sinks/Scope', [mdl '/Duty'], ...
    'Position', [330 170 380 230]);
add_block('simulink/Sinks/To Workspace', [mdl '/Log'], ...
    'Position', [330 270 410 310], ...
    'VariableName', 'fs_log', 'SaveFormat', 'Timeseries');
add_block('simulink/Sinks/Display', [mdl '/Latest'], ...
    'Position', [330 350 410 390]);

add_line(mdl, 'Source/1', 'Demux/1', 'autorouting', 'on');
% The Scope legend uses the signal names, so name the three lines that feed
% the scopes.  Without this the legend would show blank or generic entries.
hTemp = add_line(mdl, 'Demux/2', 'Temperature/1', 'autorouting', 'on');
set_param(hTemp, 'Name', 'Temperature');
hSetpoint = add_line(mdl, 'Demux/3', 'Temperature/2', 'autorouting', 'on');
set_param(hSetpoint, 'Name', 'Setpoint');
hDuty = add_line(mdl, 'Demux/4', 'Duty/1', 'autorouting', 'on');
set_param(hDuty, 'Name', 'Duty');
add_line(mdl, 'Source/1', 'Log/1', 'autorouting', 'on');
add_line(mdl, 'Source/1', 'Latest/1', 'autorouting', 'on');

% Scroll rather than sweep, keep both scopes on the same time span, and open
% them automatically when the simulation starts.
try
    set_param([mdl '/Temperature'], 'TimeSpan', num2str(timeSpan));
    set_param([mdl '/Temperature'], 'TimeSpanOverrunAction', 'Scroll');
catch
end
try
    set_param([mdl '/Duty'], 'TimeSpan', num2str(timeSpan));
    set_param([mdl '/Duty'], 'TimeSpanOverrunAction', 'Scroll');
catch
end
setIf([mdl '/Temperature'], {'OpenAtSimulationStart'}, 'on');
setIf([mdl '/Duty'], {'OpenAtSimulationStart'}, 'on');

% Legends come from the signal names set above.  YLimits is applied before
% AxesScaling because setting manual limits after Auto flips the scope back
% to Manual.  The initial limits show the full expected range, then Auto fits
% the arriving data without pressing Scale Y-axis Limits.
configureScope([mdl '/Temperature'], [0 300], 'Temperature (C)', 'Hotend temperature');
configureScope([mdl '/Duty'], [0 255], 'Duty (0-255)', 'Heater duty');

set_param(mdl, 'Solver', 'FixedStepDiscrete');
set_param(mdl, 'FixedStep', num2str(dt));
set_param(mdl, 'StopTime', 'inf');

% The MATLAB System block is interpreted, so the model must run in normal
% mode.  Pacing then pins simulation time to wall time.
try
    set_param(mdl, 'SimulationMode', 'normal');
catch err
    warning('build_live_scope:simulationMode', ...
        'Could not set SimulationMode to normal: %s', err.message);
end
try
    set_param(mdl, 'EnablePacing', 'on');
    set_param(mdl, 'PacingRate', '1');
    fprintf('Simulation pacing enabled at 1x real time.\n');
catch err
    warning('build_live_scope:pacing', ...
        ['Could not enable simulation pacing (%s). Run ' ...
         'set_param(''%s'',''EnablePacing'',''on'') in the MATLAB Command ' ...
         'Window before pressing Run.'], err.message, mdl);
end

if p.Results.Save
    save_system(mdl);
    fprintf('Saved %s.slx\n', mdl);
end

fprintf('Model %s is ready.\n', mdl);
fprintf('  Port: %s    Baud: 921600    Sample time: %.2f s\n', portName, dt);
fprintf('  Pacing: 1x real time. The scopes advance with wall time.\n');
fprintf('  Press Run. The Source block retries STX until frames arrive.\n');
fprintf('  Stop, then run fruitsnack_log once (or send 0x03) to restore ASCII.\n');
end

function configureScope(blk, yLimits, yLabel, titleText)
%CONFIGURESCOPE  Add a legend and readable, auto-scaling axes.
cfg = get_param(blk, 'ScopeConfiguration');
cfg.ActiveDisplay = 1;
cfg.YLimits = yLimits;
cfg.ShowLegend = true;
cfg.YLabel = yLabel;
cfg.Title = titleText;
cfg.TimeUnits = 'Seconds';

% Set this last: assigning YLimits after AxesScaling forces Manual mode.
cfg.AxesScaling = 'Auto';

if ~strcmpi(cfg.AxesScaling, 'Auto')
    warning('build_live_scope:scopeScaling', ...
        'Could not enable auto-scaling on %s; use Scale Y-axis Limits manually.', blk);
end
end

function ok = setIf(blk, names, value)
%SETIF  Try several mask/block parameter names, succeeding on the first one.
ok = false;
for i = 1:numel(names)
    try
        set_param(blk, names{i}, value);
        ok = true;
        return;
    catch
    end
end
end
