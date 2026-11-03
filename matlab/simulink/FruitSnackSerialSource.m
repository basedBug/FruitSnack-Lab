classdef FruitSnackSerialSource < matlab.System
    %FRUITSNAKSerialSource  Read FruitSnack binary telemetry from a serial port.
    %
    %   Used by the Simulink MATLAB System block.  The block is non-blocking:
    %   each step drains whatever bytes are available, accumulates them, and
    %   parses complete 18-byte frames:
    %
    %       uint8 0xAA, uint8 0x55,
    %       single seconds, single temp_c, single setpoint_c, single duty
    %
    %   It returns NaN(4,1) until a valid frame arrives, so a missing device
    %   does not freeze the simulation or produce a fake zero trace.
    %
    %   Opening a serial port can reset the ESP32 through DTR/RTS, and the
    %   firmware boots in ASCII mode.  The object therefore retries STX until
    %   the first frame arrives, and again after a long silence.  ETX (0x03)
    %   is sent when the simulation stops, restoring the ASCII telemetry used
    %   by fruitsnack_log.
    %
    %   Pair this with simulation pacing (see build_live_scope.m).  Without
    %   pacing, Simulink runs the fixed-step loop as fast as MATLAB can execute
    %   it, so scope time races ahead of wall time.

    properties (Nontunable)
        Port = '';
        Baud = 921600;
        Timeout = 2;
    end

    properties (Access = private)
        SerialPortObj = [];
        Buffer = uint8([]);
        LastOutput = NaN(4, 1);
        FrameCount = 0;
        LastFrameTic = [];
        LastStxTic = [];
        BytesSeen = false;
        Warned = false;
        ReadErrorWarned = false;
    end

    methods (Access = protected)
        function setupImpl(obj)
            if isempty(obj.Port)
                error('FruitSnackSerialSource:noPort', ...
                    'Port is empty. Open the Source block and set Port to the UART COM port.');
            end

            obj.SerialPortObj = serialport(obj.Port, obj.Baud, ...
                "Timeout", obj.Timeout);

            % Keep DTR/RTS deasserted after opening.  On the ESP32 DevKitC-1
            % these lines drive the auto-reset circuit; leaving them asserted
            % can hold the board in reset or in the bootloader.
            try
                setDTR(obj.SerialPortObj, false);
            catch
            end
            try
                setRTS(obj.SerialPortObj, false);
            catch
            end

            try
                flush(obj.SerialPortObj);
            catch
            end

            obj.Buffer = uint8([]);
            obj.LastOutput = NaN(4, 1);
            obj.FrameCount = 0;
            obj.BytesSeen = false;
            obj.Warned = false;
            obj.ReadErrorWarned = false;
            obj.LastFrameTic = tic;
            obj.LastStxTic = tic;

            obj.sendStx();
            fprintf('FruitSnackSerialSource: opening %s @ %d baud\n', ...
                obj.Port, obj.Baud);
        end

        function out = stepImpl(obj)
            gotBytes = false;

            try
                n = obj.SerialPortObj.NumBytesAvailable;
                if n > 0
                    data = read(obj.SerialPortObj, n, "uint8");
                    if ~isempty(data)
                        obj.Buffer = [obj.Buffer, data(:)'];
                        obj.BytesSeen = true;
                        gotBytes = true;
                    end
                end
            catch err
                if ~obj.ReadErrorWarned
                    warning('FruitSnackSerialSource:readFailed', ...
                        'Serial read failed on %s: %s', obj.Port, err.message);
                    obj.ReadErrorWarned = true;
                end
            end

            parsed = false;
            while numel(obj.Buffer) >= 18
                idx = find(obj.Buffer(1:end-1) == uint8(170) & ...
                           obj.Buffer(2:end) == uint8(85), 1);
                if isempty(idx)
                    % No header: keep one byte in case the next byte completes
                    % a split header, and discard the rest of the stale data.
                    obj.Buffer = obj.Buffer(max(1, numel(obj.Buffer) - 1):end);
                    break;
                end
                if idx > 1
                    obj.Buffer(1:idx-1) = [];
                end
                if numel(obj.Buffer) < 18
                    break;
                end

                payload = obj.Buffer(3:18);
                values = typecast(payload, "single");
                out = double(values(:));
                obj.LastOutput = out;
                obj.FrameCount = obj.FrameCount + 1;
                obj.LastFrameTic = tic;
                parsed = true;
                obj.Buffer(1:18) = [];

                if obj.FrameCount == 1
                    fprintf(['FruitSnackSerialSource: first frame: ' ...
                        't=%.2f s, temp=%.2f C, setpoint=%.2f C, duty=%.0f\n'], ...
                        out(1), out(2), out(3), out(4));
                elseif mod(obj.FrameCount, 40) == 0
                    fprintf(['FruitSnackSerialSource: %d frames, ' ...
                        't=%.2f s, temp=%.2f C, setpoint=%.2f C, duty=%.0f\n'], ...
                        obj.FrameCount, out(1), out(2), out(3), out(4));
                end
            end

            if parsed
                obj.Warned = false;
            else
                % The firmware ignores STX while it is already in binary
                % mode, so re-sending is harmless.  It recovers the board
                % after an open/reset or a stop that missed ETX.
                silent = toc(obj.LastFrameTic);
                if obj.FrameCount == 0 || silent > 2
                    if toc(obj.LastStxTic) > 0.5
                        obj.sendStx();
                        obj.LastStxTic = tic;
                    end
                end

                if ~obj.Warned && silent > 8
                    if obj.FrameCount > 0
                        warning('FruitSnackSerialSource:stale', ...
                            'No frames on %s for %.1f s. Check the UART cable and that the device is running.', ...
                            obj.Port, silent);
                    elseif gotBytes || obj.BytesSeen
                        warning('FruitSnackSerialSource:noFrame', ...
                            ['Bytes are arriving on %s but no 0xAA 0x55 frames were found. ' ...
                             'Check that the 18-byte binary firmware is running, and that ' ...
                             'this is the UART port rather than native USB.'], obj.Port);
                    else
                        warning('FruitSnackSerialSource:noData', ...
                            ['No serial data on %s @ %d baud. Check that this is the UART ' ...
                             'USB port (not native USB), that the 18-byte firmware is ' ...
                             'flashed, and that no monitor owns the port.'], ...
                            obj.Port, obj.Baud);
                    end
                    obj.Warned = true;
                end
            end

            % Hold the last sample briefly so a single missed frame does not
            % flicker the scope, then output NaN so a dead stream is visible
            % as a gap instead of a frozen line.
            if obj.FrameCount > 0 && toc(obj.LastFrameTic) <= 2
                out = obj.LastOutput;
            else
                out = NaN(4, 1);
            end
        end

        function releaseImpl(obj)
            if ~isempty(obj.SerialPortObj)
                try
                    write(obj.SerialPortObj, uint8(3), "uint8");
                catch
                end
                try
                    delete(obj.SerialPortObj);
                catch
                end
                obj.SerialPortObj = [];
            end
        end

        function sendStx(obj)
            if isempty(obj.SerialPortObj)
                return;
            end
            try
                write(obj.SerialPortObj, uint8(2), "uint8");
            catch
            end
        end

        function num = getNumOutputsImpl(~)
            num = 1;
        end

        function sz = getOutputSizeImpl(~)
            sz = [4 1];
        end

        function dt = getOutputDataTypeImpl(~)
            dt = 'double';
        end

        function c = isOutputComplexImpl(~)
            c = false;
        end

        function f = isOutputFixedSizeImpl(~)
            f = true;
        end
    end
end
