Texture1D<float4> IniParams : register(t120);
Texture2D<float4> tex : register(t0);
SamplerState samp : register(s0);

// test.dds has a red, a green, a blue and a white quarter. x comes from the
// store command and y from the if/else on $show, so the screen is tinted
// (0.75, 1, 1) while $show is set and (0.75, 0.25, 1) while it is not.
void main(float4 pos : SV_Position, float2 uv : TEXCOORD0, out float4 color : SV_Target0)
{
	float4 params = IniParams.Load(int2(0, 0));
	color = float4(tex.Sample(samp, uv).rgb * float3(params.x, params.y, 1), 0.5);
}
