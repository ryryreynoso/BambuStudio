import Foundation

/// The small part of MQTT 3.1.1 the Bambu LAN connection needs: CONNECT, SUBSCRIBE, PUBLISH (QoS 0),
/// PINGREQ and DISCONNECT out; CONNACK, SUBACK, PUBLISH and PINGRESP in.
enum MQTTPacket {
    enum PacketType: UInt8 {
        case connect = 1, connack = 2, publish = 3, subscribe = 8, suback = 9, pingreq = 12, pingresp = 13, disconnect = 14
    }

    enum Incoming: Equatable {
        case connack(returnCode: UInt8)
        case suback(packetID: UInt16, codes: [UInt8])
        case publish(topic: String, payload: Data, retained: Bool = false)
        case pingresp
        case other(type: UInt8)
    }

    // MARK: Encoding

    static func connect(clientID: String, username: String, password: String, keepAlive: UInt16) -> Data {
        var variable = Data()
        variable.append(string: "MQTT")
        variable.append(4)                       // protocol level 3.1.1
        variable.append(0b1100_0010)             // username, password, clean session
        variable.append(UInt8(keepAlive >> 8))
        variable.append(UInt8(keepAlive & 0xff))
        variable.append(string: clientID)
        variable.append(string: username)
        variable.append(string: password)
        return packet(type: .connect, flags: 0, body: variable)
    }

    static func subscribe(packetID: UInt16, topic: String) -> Data {
        var body = Data()
        body.append(UInt8(packetID >> 8))
        body.append(UInt8(packetID & 0xff))
        body.append(string: topic)
        body.append(0)                           // QoS 0
        return packet(type: .subscribe, flags: 0b0010, body: body)
    }

    static func publish(topic: String, payload: Data) -> Data {
        var body = Data()
        body.append(string: topic)
        body.append(payload)
        return packet(type: .publish, flags: 0, body: body)
    }

    static let pingreq = Data([PacketType.pingreq.rawValue << 4, 0])
    static let disconnect = Data([PacketType.disconnect.rawValue << 4, 0])

    static func packet(type: PacketType, flags: UInt8, body: Data) -> Data {
        var data = Data([type.rawValue << 4 | flags])
        data.append(encodeRemainingLength(body.count))
        data.append(body)
        return data
    }

    static func encodeRemainingLength(_ length: Int) -> Data {
        var value = length
        var out = Data()
        repeat {
            var byte = UInt8(value % 128)
            value /= 128
            if value > 0 { byte |= 0x80 }
            out.append(byte)
        } while value > 0
        return out
    }

    // MARK: Decoding

    /// Removes and returns every complete packet at the start of `buffer`; a partial packet stays.
    static func decode(_ buffer: inout Data) throws -> [Incoming] {
        var packets: [Incoming] = []
        while true {
            guard let (header, length, bodyStart) = try readFixedHeader(buffer) else { break }
            guard buffer.count >= bodyStart + length else { break }
            let start = buffer.startIndex
            let body = buffer.subdata(in: (start + bodyStart)..<(start + bodyStart + length))
            buffer.removeSubrange(start..<(start + bodyStart + length))
            packets.append(try parse(header: header, body: body))
        }
        return packets
    }

    enum DecodeError: Error { case malformedLength, malformedPacket }

    /// Returns (first byte, remaining length, offset of the body) or nil if more bytes are needed.
    static func readFixedHeader(_ buffer: Data) throws -> (UInt8, Int, Int)? {
        guard let header = buffer.first else { return nil }
        var multiplier = 1
        var length = 0
        var index = 1
        while true {
            guard index < buffer.count else { return nil }
            let byte = buffer[buffer.startIndex + index]
            length += Int(byte & 0x7f) * multiplier
            index += 1
            if byte & 0x80 == 0 { break }
            multiplier *= 128
            if index > 4 { throw DecodeError.malformedLength }
        }
        guard length <= 1024 * 1024 else { throw DecodeError.malformedLength }
        return (header, length, index)
    }

    static func parse(header: UInt8, body: Data) throws -> Incoming {
        let type = header >> 4
        switch type {
        case PacketType.connack.rawValue:
            guard body.count >= 2 else { throw DecodeError.malformedPacket }
            return .connack(returnCode: body[body.startIndex + 1])
        case PacketType.suback.rawValue:
            guard body.count == 3 else { throw DecodeError.malformedPacket }
            return .suback(packetID: UInt16(body[body.startIndex]) << 8 | UInt16(body[body.startIndex + 1]), codes: [body[body.startIndex + 2]])
        case PacketType.pingresp.rawValue:
            return .pingresp
        case PacketType.publish.rawValue:
            guard body.count >= 2 else { throw DecodeError.malformedPacket }
            let topicLength = Int(body[body.startIndex]) << 8 | Int(body[body.startIndex + 1])
            var offset = 2 + topicLength
            guard body.count >= offset else { throw DecodeError.malformedPacket }
            let topic = String(decoding: body.subdata(in: (body.startIndex + 2)..<(body.startIndex + offset)), as: UTF8.self)
            let qos = (header >> 1) & 0b11
            guard qos == 0 else { throw DecodeError.malformedPacket }
            if qos > 0 { offset += 2 }           // packet identifier
            guard body.count >= offset else { throw DecodeError.malformedPacket }
            return .publish(topic: topic, payload: body.subdata(in: (body.startIndex + offset)..<body.endIndex), retained: header & 1 != 0)
        default:
            return .other(type: type)
        }
    }
}

extension Data {
    /// Appends an MQTT UTF-8 string: two-byte big-endian length, then the bytes.
    mutating func append(string: String) {
        let bytes = Array(string.utf8)
        append(UInt8(bytes.count >> 8))
        append(UInt8(bytes.count & 0xff))
        append(contentsOf: bytes)
    }
}
