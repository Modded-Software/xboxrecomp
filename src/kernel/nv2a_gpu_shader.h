#ifndef NV2A_GPU_SHADER_H
#define NV2A_GPU_SHADER_H

static const char nv2a_gpu_shader_source[] = R"HLSL(
cbuffer State : register(b0) {
 uint4 control; uint4 misc; uint4 stages[8]; uint4 finalInput;
 uint4 textureControl;
 float4 factors[18]; float4 fogColor; float4 viewport; float4 textureInfo[4];
 float4 bumpMatrix[4]; float4 bumpLuminance[4];
 float4 fogParameters;
 uint4 fogControl;
 float4 fogPlane;
 uint4 textureKey[4];
 float4 depthRange;
 float4 depthOffset;
 float4 shaderEyeVector;
 float4 vertexConstants[192];
 uint4 lightState;      /* x=lighting_enable, y=specular_enable, z=light_enable_mask, w=color_material */
 uint4 lightControl;    /* flags, normalization, skin mode, texgen viewer */
 float4 lightMaterial;  /* xyz=emission, w=alpha */
 float4 sceneAmbient;   /* xyz */
 float4 specularPower;  /* x=specular exponent */
 float4 lightAmbient[8];
 float4 lightDiffuse[8];
 float4 lightSpecular[8];
 float4 lightLocalPosition[8];    /* xyz=position, w=range */
 float4 lightLocalAttenuation[8]; /* xyz=constant/linear/quadratic */
 float4 lightInfiniteDirection[8];
 float4 lightInfiniteHalfVector[8];
 float4 lightSpotDirection[8];    /* xyz=direction, w=spot parameter */
 uint4 texgen[4]; uint4 textureMatrixEnable;
 uint4 windowClipControl; uint4 windowClipRects[8];
};
#ifndef NV_WINDOW_CLIP_MODE
#define NV_WINDOW_CLIP_MODE windowClipControl.x
#endif
#ifdef NV_COMPILED
static const uint4 compiledStages[8]=NV_STAGE_WORDS;
static const uint4 compiledTextureSigns=NV_TEXTURE_SIGNS;
#define COMBINER_STAGES compiledStages
#define TEXTURE_SIGN(stage) compiledTextureSigns[stage]
#else
#define NV_STAGE_PROGRAM control.y
#define NV_STAGE_COUNT control.x
#define NV_COMBINER_CONTROL finalInput.z
#define NV_FINAL0 finalInput.x
#define NV_FINAL1 finalInput.y
#define COMBINER_STAGES stages
#define TEXTURE_SIGN(stage) textureKey[stage].w
#endif
#ifndef NV_W_DEPTH
#define NV_W_DEPTH (misc.y&0x10000)
#define NV_DEPTH_SEMANTIC SV_DEPTH
#endif
Texture2D image0 : register(t0); Texture2D image1 : register(t1);
Texture2D image2 : register(t2); Texture2D image3 : register(t3);
TextureCube cube0 : register(t4); TextureCube cube1 : register(t5);
TextureCube cube2 : register(t6); TextureCube cube3 : register(t7);
Texture3D volume0 : register(t8); Texture3D volume1 : register(t9);
Texture3D volume2 : register(t10); Texture3D volume3 : register(t11);
SamplerState sampler0 : register(s0); SamplerState sampler1 : register(s1);
SamplerState sampler2 : register(s2); SamplerState sampler3 : register(s3);
struct Vertex {
 float4 position:POSITION; float4 diffuse:COLOR0; float4 specular:COLOR1;
 float4 tex0:TEXCOORD0; float4 tex1:TEXCOORD1; float4 tex2:TEXCOORD2; float4 tex3:TEXCOORD3;
 float fogCoordinate:FOG;
 float4 normal:NORMAL0;
 float4 weights:BLENDWEIGHT0;
};
struct Pixel {
 noperspective centroid float4 position:SV_POSITION; float4 diffuse:COLOR0; float4 specular:COLOR1;
 float4 tex0:TEXCOORD0; float4 tex1:TEXCOORD1; float4 tex2:TEXCOORD2; float4 tex3:TEXCOORD3;
 float depthW:TEXCOORD4;
 noperspective float depthZ:TEXCOORD5;
 float fog:TEXCOORD6;
 nointerpolation float depthSlope:TEXCOORD7;
};
float fog_factor(float distance) {
 if(textureControl.z==0)return 1;
 uint mode=textureControl.w;
 bool absolute=mode==0x802 || mode==0x803 || mode==0x804;
 float exceptional=mode==0x2601 || mode==0x804 || mode==0x800?1:0;
 if(isinf(distance))return exceptional;
 float factor;
 if(mode==0x2601 || mode==0x804)factor=fogParameters.x+distance*fogParameters.y-1;
 else if(mode==0x800 || mode==0x802)factor=fogParameters.x+exp2(distance*fogParameters.y*16)-1.5;
 else factor=fogParameters.x+exp2(-distance*distance*fogParameters.y*fogParameters.y*32)-1.5;
 if(absolute)factor=abs(factor);
 if(isnan(factor))factor=exceptional;
 return clamp(factor,-3.402823466e38,3.402823466e38);
}
float4 guest_multiply(float4 left,float4 right) {
 return float4(left.x==0||right.x==0?0:left.x*right.x,left.y==0||right.y==0?0:left.y*right.y,
               left.z==0||right.z==0?0:left.z*right.z,left.w==0||right.w==0?0:left.w*right.w);
}
float guest_clip_w(float value) {
 if(!isfinite(value))return value;
 float magnitude=clamp(abs(value),5.421010862427522e-20,1.8446744073709552e19);
 return (asuint(value)&0x80000000u)?-magnitude:magnitude;
}
float4 fixed_matrix(float4 value,uint firstRow) {
 float4 result;
 [unroll]for(uint row=0;row<4;row++) {
  float4 products=guest_multiply(value,vertexConstants[firstRow+row]);
  result[row]=products.x+products.y+products.z+products.w;
 }
 return result;
}
void fixed_eye_transform(Vertex input,out float4 position,out float3 normal) {
 uint mode=lightControl.z;
 uint count=mode==0?1:(mode+3)/2;
 float remaining=1;
 position=0; normal=0;
 [unroll]for(uint slot=0;slot<4;slot++) {
  if(slot<count) {
   float weight=mode==0?1:((mode&1)!=0 && slot==count-1?remaining:input.weights[slot]);
   remaining-=weight;
   position+=fixed_matrix(input.position,8+slot*8)*weight;
   normal+=fixed_matrix(float4(input.normal.xyz,0),12+slot*8).xyz*weight;
  }
 }
 if(lightControl.y!=0)normal=normalize(normal);
}
float4 fixed_texcoord(float4 coordinate,uint stage,float4 objectPosition,float4 eyePosition,float3 normal) {
 float3 reflected=reflect(normalize(eyePosition.xyz),normal);
 [unroll]for(uint component=0;component<4;component++) {
  uint mode=texgen[stage][component];
  if(mode==0x2400)coordinate[component]=dot(vertexConstants[64+stage*8+component],eyePosition);
  else if(mode==0x2401)coordinate[component]=dot(vertexConstants[64+stage*8+component],objectPosition);
  else if(mode==0x2402 && component<2)coordinate[component]=0.5+reflected[component]/(2*length(reflected+float3(0,0,1)));
  else if(mode==0x8511 && component<3)coordinate[component]=normal[component];
  else if(mode==0x8512 && component<3)coordinate[component]=reflected[component];
 }
 return textureMatrixEnable[stage]!=0?fixed_matrix(coordinate,68+stage*8):coordinate;
}
Pixel vs_main(Vertex input) {
 Pixel output;
 float distance=fogControl.x==6?input.fogCoordinate:saturate(input.specular.a);
 if(misc.z!=0) {
  float4 eyeSpacePosition;
  float3 eyeSpaceNormal;
  fixed_eye_transform(input,eyeSpacePosition,eyeSpaceNormal);
  if(textureControl.z!=0 && fogControl.x>=1 && fogControl.x<=3) {
   distance=fogControl.x==1?length(eyeSpacePosition.xyz):dot(fogPlane.xyz,eyeSpacePosition.xyz)+fogPlane.w;
   if(fogControl.x==3)distance=abs(distance);
  }
  input.tex0=fixed_texcoord(input.tex0,0,input.position,eyeSpacePosition,eyeSpaceNormal);
  input.tex1=fixed_texcoord(input.tex1,1,input.position,eyeSpacePosition,eyeSpaceNormal);
  input.tex2=fixed_texcoord(input.tex2,2,input.position,eyeSpacePosition,eyeSpaceNormal);
  input.tex3=fixed_texcoord(input.tex3,3,input.position,eyeSpacePosition,eyeSpaceNormal);
  float4 transformed=fixed_matrix(lightControl.z==0?input.position:eyeSpacePosition,0);
  if(lightState.x!=0) {
   float3 eyeSpacePos=eyeSpacePosition.xyz/eyeSpacePosition.w;
   bool localEye=(lightControl.x&0x10000)!=0;
   bool separateSpecular=(lightControl.x&1)!=0;
   float3 toEyeDirection=float3(0,0,0);
   if(localEye) {
    float4 eyeW=vertexConstants[56];
    toEyeDirection=normalize(eyeW.xyz/eyeW.w-eyeSpacePos);
   }
   uint emissionSrc=lightState.w&3,ambientSrc=(lightState.w>>2)&3,diffuseSrc=(lightState.w>>4)&3,specularSrc=(lightState.w>>6)&3;
   float sourceAlpha=diffuseSrc==0?lightMaterial.w:diffuseSrc==2?input.specular.a:input.diffuse.a;
   float3 ambientBase=ambientSrc==0?sceneAmbient.xyz:ambientSrc==2?input.specular.xyz:input.diffuse.xyz;
   float3 emissionBase=emissionSrc==0?sceneAmbient.xyz:emissionSrc==2?input.specular.xyz:input.diffuse.xyz;
   float3 accumDiffuse=ambientBase*lightMaterial.xyz+emissionBase;
   float3 accumSpecular=float3(0,0,0);
   [unroll]for(uint light=0;light<8;light++) {
    uint type=(lightState.z>>(light*2))&3;
    if(type==0)continue;
    float attenuation=1;
    float3 lightDirection=float3(0,0,0);
    bool contributes=true;
    if(type==2||type==3) {
     float3 toLight=lightLocalPosition[light].xyz-eyeSpacePos;
     float distanceToLight=length(toLight);
     if(distanceToLight>lightLocalPosition[light].w)contributes=false;
     else {
      lightDirection=distanceToLight>0?toLight/distanceToLight:toLight;
      float3 coefficients=lightLocalAttenuation[light].xyz;
      float denominator=coefficients.x+coefficients.y*distanceToLight+coefficients.z*distanceToLight*distanceToLight;
      attenuation=denominator!=0?1/denominator:0;
      if(type==3) {
       float4 spot=lightSpotDirection[light];
       float spotLength=length(spot.xyz);
       float inverseScale=spotLength>0?1/spotLength:0;
       float cosHalfPhi=-inverseScale*spot.w;
       float cosHalfTheta=inverseScale+cosHalfPhi;
       float spotDirDotVP=dot(spot.xyz,lightDirection);
       float rho=inverseScale*spotDirDotVP;
       if(rho<=cosHalfPhi)attenuation=0;
       else if(rho<=cosHalfTheta)attenuation*=spotDirDotVP+spot.w;
      }
     }
    } else lightDirection=normalize(lightInfiniteDirection[light].xyz);
    if(contributes && attenuation!=0) {
     float nDotVP=max(0,dot(eyeSpaceNormal,lightDirection));
     float3 halfVector=type==1 && !localEye?lightInfiniteHalfVector[light].xyz:
                        normalize(lightDirection+toEyeDirection);
     float nDotHV=max(0,dot(eyeSpaceNormal,halfVector));
     float pf=(lightState.y==0||nDotVP==0||nDotHV==0)?0:pow(nDotHV,specularPower.x);
     float3 lightAmbientTerm=lightAmbient[light].xyz*attenuation;
     float3 lightDiffuseTerm=lightDiffuse[light].xyz*attenuation*nDotVP;
     float3 lightSpecularTerm=lightSpecular[light].xyz*attenuation*pf;
     accumDiffuse+=lightAmbientTerm;
     accumDiffuse+=diffuseSrc==0?lightDiffuseTerm:diffuseSrc==1?input.diffuse.xyz*lightDiffuseTerm:input.specular.xyz*lightDiffuseTerm;
     accumSpecular+=specularSrc==0?lightSpecularTerm:specularSrc==1?input.diffuse.xyz*lightSpecularTerm:input.specular.xyz*lightSpecularTerm;
    }
   }
   if(lightState.y!=0 && !separateSpecular)accumDiffuse+=accumSpecular;
   else if(lightState.y!=0) {
    input.specular=float4(accumSpecular,input.specular.a);
   }
   input.diffuse=float4(accumDiffuse,sourceAlpha);
  }
  if(lightState.y==0)input.specular=float4(0,0,0,1);
  input.diffuse=saturate(input.diffuse); input.specular=saturate(input.specular);
  transformed.w=guest_clip_w(transformed.w);
  transformed.xyz/=transformed.w;
  transformed.xy+=vertexConstants[59].xy;
  input.position=transformed;
 }
 input.position.w=guest_clip_w(input.position.w);
 output.position=float4((2*input.position.x/viewport.x-1)*input.position.w,
                       (1-2*input.position.y/viewport.y)*input.position.w,
                       ((misc.y&0x10000)?0:input.position.z/viewport.z)*input.position.w,input.position.w);
 output.diffuse=input.diffuse; output.specular=input.specular;
 output.tex0=input.tex0; output.tex1=input.tex1; output.tex2=input.tex2; output.tex3=input.tex3;
 output.depthW=input.position.w;
 output.depthZ=input.position.z;
 output.fog=fog_factor(distance);
 output.depthSlope=0;
 return output;
}
)HLSL" R"HLSL(
[maxvertexcount(3)]
void gs_main(triangle Pixel vertices[3],inout TriangleStream<Pixel> stream) {
 [unroll]for(uint vertex=0;vertex<3;vertex++)
  if(!all(isfinite(vertices[vertex].position)))return;
 float slope=0;
 if(depthOffset.x!=0 && (depthOffset.z!=0 || depthOffset.w!=0)) {
  float2 screenPoints[3];
  [unroll]for(uint vertex=0;vertex<3;vertex++) {
   float2 normalized=vertices[vertex].position.xy/vertices[vertex].position.w;
   screenPoints[vertex]=float2(normalized.x+1,1-normalized.y)*viewport.xy*0.5;
  }
  float3 depths=(misc.y&0x10000)?rcp(float3(vertices[0].depthW,vertices[1].depthW,vertices[2].depthW)):
                               float3(vertices[0].depthZ,vertices[1].depthZ,vertices[2].depthZ);
  float3 firstEdge=float3(screenPoints[1]-screenPoints[0],depths.y-depths.x);
  float3 secondEdge=float3(screenPoints[2]-screenPoints[0],depths.z-depths.x);
  float3 plane=cross(firstEdge,secondEdge);
  if(plane.z!=0)slope=max(abs(plane.x),abs(plane.y))/abs(plane.z);
  if(!isfinite(slope))slope=0;
 }
 [unroll]for(uint vertex=0;vertex<3;vertex++) {
  Pixel result=vertices[vertex];
  result.depthSlope=slope;
  if(misc.w!=0) {result.diffuse=vertices[2].diffuse;result.specular=vertices[2].specular;}
  stream.Append(result);
 }
 stream.RestartStrip();
}
[maxvertexcount(2)]
void gs_lines_main(line Pixel vertices[2],inout LineStream<Pixel> stream) {
 [unroll]for(uint vertex=0;vertex<2;vertex++)
  if(!all(isfinite(vertices[vertex].position)))return;
 [unroll]for(uint vertex=0;vertex<2;vertex++) {
  Pixel result=vertices[vertex];
  if(misc.w!=0) {result.diffuse=vertices[1].diffuse;result.specular=vertices[1].specular;}
  stream.Append(result);
 }
 stream.RestartStrip();
}
[maxvertexcount(1)]
void gs_points_main(point Pixel vertices[1],inout PointStream<Pixel> stream) {
 if(all(isfinite(vertices[0].position)))stream.Append(vertices[0]);
}
float4 mapped(float4 registers[16], uint encoded, bool alpha) {
 encoded &=255; float4 value=registers[encoded&15];
 if(encoded&16) value=value.aaaa; else if(alpha) value=value.bbbb;
 switch((encoded>>5)&7) {
 case 0:return max(value,0); case 1:return 1-saturate(value);
 case 2:return 2*max(value,0)-1; case 3:return 1-2*max(value,0);
 case 4:return max(value,0)-0.5; case 5:return 0.5-max(value,0);
 case 6:return value; default:return -value;
 }
}
float4 output_map(float4 value,uint encoded) {
 switch((encoded>>15)&7) {
 case 1:value-=0.5;break; case 2:value*=2;break; case 3:value=(value-0.5)*2;break;
 case 4:value*=4;break; case 6:value*=0.5;break;
 }
 return clamp(value,-1,1);
}
void products(float4 registers[16],uint input,uint output,bool alpha,bool select,
              out float4 ab,out float4 cd,out float4 sum) {
 ab=mapped(registers,input>>24,alpha)*mapped(registers,input>>16,alpha);
 cd=mapped(registers,input>>8,alpha)*mapped(registers,input,alpha);
 if(!alpha && (output&0x2000)) ab=dot(ab.rgb,float3(1,1,1));
 if(!alpha && (output&0x1000)) cd=dot(cd.rgb,float3(1,1,1));
 sum=(output&0x4000)?(select?cd:ab):ab+cd;
 ab=output_map(ab,output); cd=output_map(cd,output); sum=output_map(sum,output);
}
void store(inout float4 registers[16],uint output,bool alpha,float4 ab,float4 cd,float4 sum) {
 uint destinations[3]={(output>>4)&15,output&15,(output>>8)&15};
 float4 values[3]={ab,cd,sum};
 [unroll]for(uint product=0;product<3;product++) {
  uint destination=destinations[product]; if(!destination)continue;
  if(alpha) registers[destination].a=values[product].x;
  else {
   registers[destination].rgb=values[product].rgb;
   if(product<2 && (output&(product==0?0x80000:0x40000))) registers[destination].a=values[product].b;
  }
 }
}
bool compare_alpha(uint function,uint incoming,uint reference) {
 switch(function) {
 case 0x200:return false; case 0x201:return incoming<reference; case 0x202:return incoming==reference;
 case 0x203:return incoming<=reference; case 0x204:return incoming>reference; case 0x205:return incoming!=reference;
 case 0x206:return incoming>=reference; default:return true;
 }
}
struct PixelOutput { float4 color:SV_TARGET; float depth:NV_DEPTH_SEMANTIC; };
void window_clip_pixel(float2 position) {
 if(NV_WINDOW_CLIP_MODE!=0) {
  float2 coordinate=position-0.5;
  bool contained=false;
  [unroll]for(uint rectangle=0;rectangle<8;rectangle++) {
   if(rectangle<windowClipControl.y)
    contained=contained || (all(coordinate>=float2(windowClipRects[rectangle].xy)) &&
                            all(coordinate<float2(windowClipRects[rectangle].zw)));
  }
  if((NV_WINDOW_CLIP_MODE==1 && !contained) || (NV_WINDOW_CLIP_MODE==2 && contained))discard;
 }
}
PixelOutput finish_pixel(float4 color,Pixel input,bool frontFacing) {
 window_clip_pixel(input.position.xy);
 color=saturate(color);
 if(control.z && !compare_alpha(control.w,(uint)(color.a*255+0.5),misc.x))discard;
 PixelOutput output;
 output.color=color;
 float depth=NV_W_DEPTH?input.depthW:input.depthZ;
 if(NV_W_DEPTH && (!(depth>0) || !isfinite(depth)))depth=3.402823466e38;
 else if((frontFacing?depthOffset.z:depthOffset.w)!=0) {
  float slopeOffset=0;
  if(depthOffset.x!=0) {
   slopeOffset=depthOffset.x*input.depthSlope;
   if(NV_W_DEPTH) {slopeOffset*=depth; slopeOffset*=depth;}
  }
  depth+=depthOffset.y;
  depth+=slopeOffset;
  if(NV_W_DEPTH && isnan(depth))depth=3.402823466e38;
 }
 if(depthRange.z==0) {
  if(depth<depthRange.x || depth>depthRange.y)discard;
 } else depth=clamp(depth,depthRange.x,depthRange.y);
#if defined(NV_DEPTH_ENABLED) && !NV_DEPTH_ENABLED
 output.depth=0;
#else
 output.depth=floor(clamp(depth,0,viewport.z))/viewport.z;
#endif
 return output;
}
PixelOutput ps_uncombined(Pixel input,bool frontFacing:SV_IsFrontFace) {
 return finish_pixel(input.diffuse,input,frontFacing);
}
float2 cube_plane_coordinates(float3 direction) {
 float3 magnitude=abs(direction);
 uint axis=magnitude.x>magnitude.y && magnitude.x>magnitude.z?0:
           magnitude.y>magnitude.x && magnitude.y>magnitude.z?1:2;
 float horizontal=axis==0?(direction.x>0?-direction.z:direction.z):
                  axis==1?direction.x:(direction.z>0?direction.x:-direction.x);
 float vertical=axis==1?(direction.y>0?-direction.z:direction.z):direction.y;
 return float2(horizontal,vertical)/magnitude[axis];
}
float3 dot_texture_channels(float4 color,uint mapping) {
 if(mapping==0)return color.rgb;
 if(mapping==4) {
  uint4 bytes=(uint4)(color*255);
  uint high=(bytes.a<<8)|bytes.r;
  uint low=(bytes.g<<8)|bytes.b;
  return float3(float(high)/65535,float(low)/65535,1);
 }
 float3 encoded=color.rgb*255;
 if(mapping==1)return (encoded-128)/127;
 float3 result;
 [unroll]for(uint component=0;component<3;component++) {
  float channel=encoded[component];
  if(mapping==2)result[component]=(channel>=128?channel-255.5:channel+0.5)/127.5;
  else result[component]=(channel>=128?channel-256:channel)/127;
 }
 return result;
}
float4 texture_color_sign(float4 color,uint mask) {
 if(mask&2)color.r=2*color.r-1;
 if(mask&4)color.g=2*color.g-1;
 if(mask&8)color.b=2*color.b-1;
 if(mask&1)color.a=2*color.a-1;
 return color;
}
PixelOutput ps_main(Pixel input,bool frontFacing:SV_IsFrontFace) {
 float4 registers[16]; [unroll]for(uint index=0;index<16;index++)registers[index]=0;
 registers[4]=input.diffuse; registers[5]=input.specular; registers[3]=float4(fogColor.rgb,saturate(input.fog));
 float4 coordinates[4]={input.tex0,input.tex1,input.tex2,input.tex3};
 float dotValues[4]={0,0,0,0};
 [unroll]for(uint stage=0;stage<4;stage++) {
  uint mode=(NV_STAGE_PROGRAM>>(stage*5))&31;
  float4 coordinate=coordinates[stage]; float4 value=float4(0,0,0,1);
  if(mode==4)value=coordinate;
  if(mode==9 || mode==11 || mode==12 || mode==14 || mode==17 || mode==18) {
   uint sourceStage=stage==1?0:(textureControl.x>>(stage*4+8))&15;
   uint mapping=(textureControl.y>>((stage-1)*4))&15;
  dotValues[stage]=dot(coordinate.xyz,dot_texture_channels(registers[8+sourceStage],mapping));
  if(mode==11) {
   uint nextSource=(textureControl.x>>20)&15;
   uint nextMapping=(textureControl.y>>8)&15;
  dotValues[3]=dot(coordinates[3].xyz,dot_texture_channels(registers[8+nextSource],nextMapping));
  }
   if(mode==17)value=0;
  }
  if(mode==5) {
   [unroll]for(uint component=0;component<4;component++) {
    bool nonnegative=(finalInput.w&(1u<<(stage*4+component)))!=0;
    if(nonnegative?coordinate[component]>=0:coordinate[component]<0)discard;
   }
   value=0;
  }
  uint textureFlags=(uint)textureInfo[stage].w;
  float3 sampleDirection=coordinate.xyz;
  if(mode==11 || mode==12 || mode==14 || mode==18) {
   sampleDirection=float3(dotValues[1],dotValues[2],dotValues[3]);
  if(mode==12 || mode==18) {
   float3 eye=mode==18?shaderEyeVector.xyz:float3(coordinates[1].w,coordinates[2].w,coordinates[3].w);
    sampleDirection=2*sampleDirection*dot(sampleDirection,eye)/dot(sampleDirection,sampleDirection)-eye;
   }
  }
  if((mode==1 || mode==3 || mode==6 || mode==7 || mode==9 || mode==11 || mode==12 || mode==14 || mode==15 || mode==16 || mode==18) && !(textureFlags&2)) {
   float2 uv=mode==3 || mode==11 || mode==12 || mode==14 || mode==18?cube_plane_coordinates(sampleDirection):coordinate.xy/coordinate.w;
     if(mode==6 || mode==7) {
        uint sourceStage=stage==1?0:(textureControl.x>>(stage*4+8))&15;
        float2 perturbation=dot_texture_channels(registers[8+sourceStage],3).bg;
        uint sourceSign=TEXTURE_SIGN(sourceStage);
        if(sourceSign&8)perturbation.x=registers[8+sourceStage].b;
        if(sourceSign&4)perturbation.y=registers[8+sourceStage].g;
        uv=coordinate.xy+float2(dot(bumpMatrix[stage].xz,perturbation),dot(bumpMatrix[stage].yw,perturbation));
     }
   if(mode==9)uv=float2(dotValues[stage-1],dotValues[stage]);
   if(mode==15 || mode==16) {
    uint sourceStage=stage==1?0:(textureControl.x>>(stage*4+8))&15;
    uv=mode==15?registers[8+sourceStage].ar:registers[8+sourceStage].gb;
   }
   if(textureInfo[stage].z!=0)uv/=textureInfo[stage].xy;
  float2 gradientX=ddx(uv),gradientY=ddy(uv);
  if(textureFlags&4)uv.x=saturate(uv.x);
  if(textureFlags&8)uv.y=saturate(uv.y);
   switch(stage) {
  case 0:value=image0.SampleGrad(sampler0,uv,gradientX,gradientY);break;
  case 1:value=image1.SampleGrad(sampler1,uv,gradientX,gradientY);break;
  case 2:value=image2.SampleGrad(sampler2,uv,gradientX,gradientY);break;
  default:value=image3.SampleGrad(sampler3,uv,gradientX,gradientY);break;
   }
  }
  if((mode==1 || mode==3 || mode==11 || mode==12 || mode==14 || mode==18) && (textureFlags&2)) {
   float3 direction=mode==1?float3(1,coordinate.y/coordinate.w,-coordinate.x/coordinate.w):sampleDirection;
   switch(stage) {
  case 0:value=cube0.Sample(sampler0,direction);break;
  case 1:value=cube1.Sample(sampler1,direction);break;
  case 2:value=cube2.Sample(sampler2,direction);break;
  default:value=cube3.Sample(sampler3,direction);break;
   }
  }
  if(mode==2) {
   float3 uvw=coordinate.xyz/coordinate.w;
   float3 gradientX=ddx(uvw),gradientY=ddy(uvw);
   if(textureFlags&4)uvw.x=saturate(uvw.x);
   if(textureFlags&8)uvw.y=saturate(uvw.y);
   if(textureFlags&32)uvw.z=saturate(uvw.z);
   switch(stage) {
   case 0:value=volume0.SampleGrad(sampler0,uvw,gradientX,gradientY);break;
   case 1:value=volume1.SampleGrad(sampler1,uvw,gradientX,gradientY);break;
   case 2:value=volume2.SampleGrad(sampler2,uvw,gradientX,gradientY);break;
   default:value=volume3.SampleGrad(sampler3,uvw,gradientX,gradientY);break;
   }
  }
    if(textureFlags&1)value.a=1;
    value=texture_color_sign(value,TEXTURE_SIGN(stage));
    if(mode==7) {
     uint sourceStage=stage==1?0:(textureControl.x>>(stage*4+8))&15;
     value*=registers[8+sourceStage].r*bumpLuminance[stage].x+bumpLuminance[stage].y;
    }
    if(mode==1 || mode==2 || mode==3 || mode==6 || mode==7 || mode==9 || mode==11 || mode==12 || mode==14 || mode==15 || mode==16 || mode==18) {
     if((textureFlags&16) && value.a==0)discard;
      uint keyMode=textureKey[stage].z;
      if(keyMode!=0) {
       uint4 channels=(uint4)(saturate(value)*255+0.5);
       uint encoded=(channels.a<<24)|(channels.r<<16)|(channels.g<<8)|channels.b;
       uint mask=textureKey[stage].y;
       if((encoded&mask)==(textureKey[stage].x&mask)) {
        if(keyMode==3)discard;
        else if(keyMode==2)value=0;
        else value.a=0;
       }
      }
    }
  registers[8+stage]=value;
 }
 registers[12].a=registers[8].a;
 [unroll]for(uint stage=0;stage<NV_STAGE_COUNT;stage++) {
  registers[1]=factors[((NV_COMBINER_CONTROL&0x1000)?stage:0)*2];
  registers[2]=factors[((NV_COMBINER_CONTROL&0x10000)?stage:0)*2+1];
  bool select=(NV_COMBINER_CONTROL&0x100)?registers[12].a>=0.5:(((uint)(saturate(registers[12].a)*255)&1)!=0);
  float4 rgbAB,rgbCD,rgbSum,alphaAB,alphaCD,alphaSum;
  products(registers,COMBINER_STAGES[stage].x,COMBINER_STAGES[stage].z,false,select,rgbAB,rgbCD,rgbSum);
  products(registers,COMBINER_STAGES[stage].y,COMBINER_STAGES[stage].w,true,select,alphaAB,alphaCD,alphaSum);
  store(registers,COMBINER_STAGES[stage].z,false,rgbAB,rgbCD,rgbSum);
  store(registers,COMBINER_STAGES[stage].w,true,alphaAB,alphaCD,alphaSum);
 }
 registers[1]=factors[16];registers[2]=factors[17];
 float4 specular=registers[5],temporary=registers[12];
 if(NV_FINAL1&0x40)specular=1-specular;
 if(NV_FINAL1&0x20)temporary=1-temporary;
 registers[14]=specular+temporary; if(NV_FINAL1&0x80)registers[14]=saturate(registers[14]);
 registers[15]=mapped(registers,NV_FINAL1>>24,false)*mapped(registers,NV_FINAL1>>16,false);
 registers[14].a=0;registers[15].a=0;
 if(NV_FINAL0==0 && NV_FINAL1==0)return finish_pixel(registers[NV_STAGE_COUNT?12:4],input,frontFacing);
 float4 aa=mapped(registers,NV_FINAL0>>24,false);
 float4 result=aa*mapped(registers,NV_FINAL0>>16,false)+(1-aa)*mapped(registers,NV_FINAL0>>8,false)+mapped(registers,NV_FINAL0,false);
 result.a=mapped(registers,NV_FINAL1>>8,true).x; result=saturate(result);
 return finish_pixel(result,input,frontFacing);
}
)HLSL";

#endif