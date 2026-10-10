void main(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0)
{
	uv = float2(id & 1, id >> 1);
	pos = float4(uv.x * 2 - 1, 1 - uv.y * 2, 0, 1);
}
